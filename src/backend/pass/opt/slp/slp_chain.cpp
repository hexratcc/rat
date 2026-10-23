#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n) {
			StoreNode* st = dyn_cast<StoreNode>(n);
			Type* t = n->getType();
			Node* ptr = n->getInput(2); // the pointer of a load or store
			if(st)
				t = st->getValue()->getType();
			Access a{addrs.get(ptr), t->byteSize(ptrBytes)};
			if(!a.size || a.size > kVecBytes || a.addr.offset > kMaxOffset || a.addr.offset < -kMaxOffset)
				a.addr = Addr{};
			return a;
		}

		StoreNode* chainSuccessor(const StoreNode* prev) {
			StoreNode* succ = nullptr;
			for(Node* u : prev->getUsers()) {
				StoreNode* st = dyn_cast<StoreNode>(u);
				if(isa<LoadNode>(u))
					continue;
				if(!st || succ || st->getControl() != prev->getControl() || st->isVolatile())
					return nullptr;
				succ = st;
			}
			return succ;
		}

		List<Chain> collectChains(Function& fn) {
			List<Chain> chains;
			for(Node* n : fn) {
				StoreNode* st = dyn_cast<StoreNode>(n);
				StoreNode* p = nullptr;
				if(st)
					p = dyn_cast<StoreNode>(st->getMemory());
				if(!st || st->isVolatile() || (p && !p->isVolatile() && chainSuccessor(p) == st))
					continue; // not a chain head
				chains.emplace_back();
				for(StoreNode* c = st; c; c = chainSuccessor(c))
					chains.back().push_back(c);
			}
			return chains;
		}

		// chain[lo..hi] have groups and share no byte, the test of a set, without a sort. written is
		// scratch: (group, 16-byte block) -> bytes written
		B32 setDisjoint(const List<Access>& acc, U32 lo, U32 hi, Map<U64, U16>& written) {
			List<U32> groups; // first index of each group
			written.clear();
			for(U32 i = lo; i <= hi; ++i) {
				const Addr& a = acc[i].addr;
				if(!a.group)
					return false;
				B32 seen = false;
				for(U32 g : groups)
					seen |= acc[g].addr.group == a.group;
				if(!seen)
					groups.push_back(i);
				if(groups.size() > kMaxSetGroups)
					return false;
				I64 end = a.offset + (I64)acc[i].size;
				for(I64 o = a.offset; o < end; o = (o | (I64)(kVecBytes - 1)) + 1) { // each 16-byte block
					I64 stop = std::min(end, (o | (I64)(kVecBytes - 1)) + 1);
					U16 bits = (U16)(((1u << (stop - o)) - 1) << (o & (kVecBytes - 1)));
					U16& mask = written[setKey(a.group, o >> kVecShift)];
					if(mask & bits)
						return false; // two stores share a byte
					mask |= bits;
				}
			}
			for(U32 x = 0; x < groups.size(); ++x)
				for(U32 y = x + 1; y < groups.size(); ++y)
					if(!disjointBases(acc[groups[x]].addr, acc[groups[y]].addr))
						return false;
			return true;
		}

		// the stores of chain[lo..hi] read the state before them, a MemMerge joins their states
		MemMergeNode* Slp::mergeStretch(const Chain& chain, U32 lo, U32 hi) {
			Node* before = chain[lo]->getMemory();
			StoreNode* last = chain[hi];
			List<Node*> ins = {last->getControl()};
			ins.insert(ins.end(), chain.begin() + lo, chain.begin() + hi + 1);
			MemMergeNode* m = fn.create<MemMergeNode>(fn.memTy(), ins);
			for(U32 k = last->getUsers().size(); k-- > 0;) // the readers of the final state
				if(Node* u = last->getUsers()[k]; u != m)
					u->setInput(last->getUserSlot(k), m);
			for(U32 i = lo + 1; i <= hi; ++i)
				chain[i]->setInput(1, before);
			return m;
		}

		// each stretch of disjoint stores whose inner states no load reads becomes a set
		List<Piece> Slp::mergeStretches() {
			List<Piece> pieces;
			List<Access> acc;
			Map<U64, U16> written;
			for(const Chain& chain : takeChains(fn)) {
				acc.clear();
				for(StoreNode* st : chain)
					acc.push_back(accessOf(addrs, ptrBytes, st));
				pieces.emplace_back();
				for(U32 a = 0, b = 0; a < chain.size(); a = b + 1) {
					for(b = a; b + 1 < chain.size() && chain[b]->getUsers().size() == 1;)
						++b;
					if(b == a || !setDisjoint(acc, a, b, written)) {
						pieces.back().chain.insert(
								pieces.back().chain.end(), chain.begin() + a, chain.begin() + b + 1);
						continue;
					}
					if(pieces.back().chain.empty())
						pieces.pop_back();
					pieces.push_back({mergeStretch(chain, a, b), {}});
					pieces.emplace_back();
				}
				if(pieces.back().chain.empty())
					pieces.pop_back();
			}
			return pieces;
		}

		// chain the stores of m in input order, give m's users the last and return it
		Node* lowerMerge(MemMergeNode* m) {
			Node* prev = nullptr;
			for(U32 i = 1; i < m->getInputCount(); ++i) {
				Node* st = m->getInput(i);
				if(st && prev)
					st->setInput(1, prev);
				if(st)
					prev = st;
			}
			for(U32 k = m->getUsers().size(); k-- > 0;)
				m->getUsers()[k]->setInput(m->getUserSlot(k), prev);
			m->clearInputs();
			return prev;
		}

		// bytes / element size stores from chain[at] of one type and group that fill one span
		B32 Slp::windowAt(const Chain& chain, U32 at, U32 bytes, Window& out) {
			Type* t = nullptr;
			if(at < chain.size())
				t = chain[at]->getValue()->getType();
			if(!(typeBits(t) & kElem))
				return false;
			U32 esz = t->byteSize(ptrBytes), w = bytes / esz;
			if(w < 2 || at + w > chain.size())
				return false;
			const Addr& a0 = addrs.get(chain[at]->getPointer());
			I64 lo = a0.offset;
			for(U32 j = 0; j < w; ++j) {
				const Addr& a = addrs.get(chain[at + j]->getPointer());
				if(chain[at + j]->getValue()->getType() != t || a.group != a0.group)
					return false;
				lo = std::min(lo, a.offset);
			}
			out = Window{at, w, bytes, lo};
			for(U32 j = 0; j < w; ++j) { // the offsets fill the w lanes above lo once each
				U64 d = (U64)(addrs.get(chain[at + j]->getPointer()).offset - lo);
				if(d % esz != 0 || d / esz >= w || out.lane[d / esz])
					return false;
				out.lane[d / esz] = chain[at + j];
			}
			return true;
		}

		U32 Slp::processChain(const Chain& chain) {
			U32 changed = 0;
			for(U32 i = 0; i < chain.size();) {
				U32 used = tryRun(chain, i, kVecBytes);
				if(!used)
					used = tryRun(chain, i, kVecBytes / 2); // 2 x 32-bit
				changed += used != 0;
				i += std::max(used, 1u);
			}
			return changed;
		}

		U64 setKey(U32 group, I64 offset) { return (U64)group << 32 | (U32)offset; }

		// the windows of a set: each run of members of one group and type whose offsets continue each
		// other, found by lookup from the member with no predecessor, no order of the members is used
		U32 Slp::processSet(MemMergeNode* m) {
			Map<U64, Pair<StoreNode*, Type*>> at; // (group, offset) -> member and its value type
			setSlot.clear();
			for(U32 i = 1; i < m->getInputCount(); ++i) {
				StoreNode* st = cast<StoreNode>(m->getInput(i));
				const Addr& a = addrs.get(st->getPointer());
				at[setKey(a.group, a.offset)] = {st, st->getValue()->getType()};
				setSlot[st] = i;
			}
			U32 changed = 0;
			Chain run;
			for(U32 i = 1; i < m->getInputCount(); ++i) {
				StoreNode* st = dyn_cast<StoreNode>(m->getInput(i));
				if(!st || !setSlot.count(st) || !(typeBits(st->getValue()->getType()) & kElem))
					continue; // a slot a pack emptied or filled
				Type* t = st->getValue()->getType();
				I64 esz = t->byteSize(ptrBytes);
				const Addr& a = addrs.get(st->getPointer());
				auto prev = at.find(setKey(a.group, a.offset - esz));
				if(prev != at.end() && prev->second.second == t)
					continue; // not the first of its run
				run.clear();
				for(auto it = at.find(setKey(a.group, a.offset)); it != at.end() && it->second.second == t;
						it = at.find(setKey(a.group, a.offset + (I64)run.size() * esz)))
					run.push_back(it->second.first);
				changed += processRun(m, run);
			}
			return changed;
		}

		// windows from the run's start on, as processChain takes them from a chain
		U32 Slp::processRun(MemMergeNode* m, const Chain& run) {
			U32 changed = 0;
			for(U32 p = 0; p < run.size();) {
				U32 used = trySetWindow(m, run, p, kVecBytes);
				if(!used)
					used = trySetWindow(m, run, p, kVecBytes / 2); // 2 x 32-bit
				changed += used != 0;
				p += std::max(used, 1u);
			}
			return changed;
		}

		U32 Slp::trySetWindow(MemMergeNode* m, const Chain& run, U32 p, U32 bytes) {
			U32 esz = run[p]->getValue()->getType()->byteSize(ptrBytes), w = bytes / esz;
			if(w < 2 || p + w > run.size())
				return 0;
			Window win{p, w, bytes, addrs.get(run[p]->getPointer()).offset};
			for(U32 k = 0; k < w; ++k)
				win.lane[k] = run[p + k];
			++stats.windowsSeen;
			if(leafProfit(win) < kMinProfit) { // no load reads a member, so no window is observed
				++stats.rejected;
				return 0;
			}
			if(!pack(setSeed(m, win)))
				return 0;
			return w;
		}

		// the windows of the lowered chains that a seam splits, as no piece held them whole
		U32 Slp::processSeams() {
			B32 inner = false; // a seam is not the last store of its chain, else no window holds one
			for(const Node* s : seams)
				inner |= chainSuccessor(cast<StoreNode>(s)) != nullptr;
			if(!inner)
				return 0;
			U32 changed = 0;
			for(const Chain& chain : collectChains(fn))
				for(U32 i = 0; i < chain.size();) {
					U32 used = 0;
					if(splitBySeam(chain, i, kVecBytes))
						used = tryRun(chain, i, kVecBytes);
					if(!used && splitBySeam(chain, i, kVecBytes / 2))
						used = tryRun(chain, i, kVecBytes / 2); // 2 x 32-bit
					changed += used != 0;
					i += std::max(used, 1u);
				}
			return changed;
		}

		// a seam is an inner store of the window of bytes at chain[i]
		B32 Slp::splitBySeam(const Chain& chain, U32 i, U32 bytes) {
			Type* t = chain[i]->getValue()->getType();
			if(!(typeBits(t) & kElem))
				return false;
			U32 w = bytes / t->byteSize(ptrBytes);
			for(U32 j = 0; j + 1 < w && i + j < chain.size(); ++j)
				if(seams.count(chain[i + j]))
					return true;
			return false;
		}

		U32 Slp::tryRun(const Chain& chain, U32 i, U32 bytes) {
			Window w0;
			if(!windowAt(chain, i, bytes, w0))
				return 0;
			++stats.windowsSeen;
			B32 observed = false;
			for(U32 j = 0; j + 1 < w0.w; ++j)
				observed |= chain[i + j]->getUsers().size() > 1; // a load besides the next store
			if((observed && guardsDisabled()) || (!observed && leafProfit(w0) < kMinProfit)) {
				++stats.rejected;
				return 0;
			}
			Run run;
			run.push_back(w0);
			if(observed)
				extendRun(chain, run);
			Seed seed = runSeed(chain, run);
			if(!pack(seed))
				return 0;
			return (U32)seed.stores.size();
		}

		// add the next windows of run[0]'s group and width that continue its bytes
		void Slp::extendRun(const Chain& chain, Run& run) {
			Window w0 = run[0], wn;
			U32 group = addrs.get(w0.lane[0]->getPointer()).group;
			while(run.size() < kMaxRunWindows) {
				if(!windowAt(chain, w0.begin + run.size() * w0.w, w0.bytes, wn))
					break;
				B32 continues = addrs.get(wn.lane[0]->getPointer()).group == group && wn.w == w0.w &&
												wn.lo == w0.lo + (I64)(run.size() * w0.bytes);
				if(!continues)
					break;
				run.push_back(wn);
				++stats.windowsSeen;
			}
		}

		I32 Slp::leafProfit(const Window& w) {
			Lanes lanes;
			B32 loads = true;
			LoadNode* l0 = dyn_cast<LoadNode>(w.lane[0]->getValue());
			for(U32 k = 0; k < w.w; ++k) {
				Node* v = w.lane[k]->getValue();
				if(isArithmeticOpcode(v->getOpcode()) || isa<SelectNode>(v))
					return INT32_MAX;
				lanes.push_back(v);
				LoadNode* l = dyn_cast<LoadNode>(v);
				loads = loads && l && l0 && l->getControl() == l0->getControl() &&
								addrs.get(l->getPointer()).group == addrs.get(l0->getPointer()).group;
			}
			if(loads)
				return INT32_MAX;
			return (I32)w.w - 1 - gatherCost(lanes);
		}

		// the run's stores and one tuple of lane values per window
		Seed runSeed(const Chain& chain, const Run& run) {
			Seed s;
			const Window& w0 = run[0];
			s.memIn = chain[w0.begin]->getMemory();
			s.windows = (U32)run.size();
			s.stores.assign(chain.data() + w0.begin, chain.data() + w0.begin + s.windows * w0.w);
			s.tuples.reserve(run.size());
			for(const Window& w : run) {
				s.heads.push_back(w.lane[0]);
				s.tuples.emplace_back();
				for(U32 k = 0; k < w.w; ++k)
					s.tuples.back().push_back(w.lane[k]->getValue());
				s.profit += (I32)w.w - 1; // the fused store
			}
			return s;
		}

		// the window's stores and its lane values, all members read the state before the set
		Seed setSeed(MemMergeNode* m, const Window& w) {
			Seed s;
			s.memIn = w.lane[0]->getMemory();
			s.set = m;
			s.stores.assign(w.lane, w.lane + w.w);
			s.heads.push_back(w.lane[0]);
			s.tuples.emplace_back();
			for(U32 k = 0; k < w.w; ++k)
				s.tuples.back().push_back(w.lane[k]->getValue());
			s.profit = (I32)w.w - 1; // the fused store
			return s;
		}

		B32 Slp::reduces(const Node* n) {
			Opcode op = n->getOpcode();
			U8 t = typeBits(n->getType());
			return ((op == Opcode::Add || (op >= Opcode::And && op <= Opcode::Xor)) && (t & kI32I64)) ||
						 (op == Opcode::Mul && t == kI32 && sse41);
		}

		U32 Slp::packReductions() {
			List<Node*> roots;
			for(Node* n : fn) {
				if(!isBinaryOpcode(n->getOpcode()))
					continue; // a cheap filter before reduces
				B32 isRoot = false;
				for(U32 i = 0; i < n->getInputCount(); ++i)
					isRoot |= n->getInput(i)->getOpcode() == n->getOpcode();
				isRoot = isRoot && reduces(n);
				for(Node* u : n->getUsers())
					isRoot = isRoot && (u->getOpcode() != n->getOpcode() || u->getType() != n->getType());
				if(isRoot)
					roots.push_back(n);
			}
			U32 changed = 0;
			for(Node* root : roots)
				changed += tryReduction(root);
			return changed;
		}

		List<Node*> flattenReduction(Node* root, List<Node*>& steps) {
			List<Node*> terms, todo = {root};
			while(!todo.empty()) {
				Node* n = todo.back();
				todo.pop_back();
				B32 step = n->getOpcode() == root->getOpcode() && n->getType() == root->getType() &&
									 (n == root || n->getUsers().size() == 1);
				if(!step || terms.size() + todo.size() >= kMaxTerms) {
					terms.push_back(n);
					continue;
				}
				steps.push_back(n);
				todo.push_back(n->getInput(0));
				todo.push_back(n->getInput(1));
			}
			std::reverse(terms.begin(), terms.end());
			return terms;
		}

		U32 Slp::tryReduction(Node* root) {
			Seed s;
			s.root = root;
			U32 w = kVecBytes / root->getType()->byteSize(ptrBytes);
			List<Node*> terms = flattenReduction(root, s.steps);
			U32 n = (U32)terms.size(), g = n / w;
			if(n < kMinTerms)
				return 0;
			++stats.windowsSeen;
			s.tuples.resize(g);
			for(U32 v = 0; v < g; ++v)
				s.tuples[v].assign(terms.data() + v * w, terms.data() + (v + 1) * w);
			s.rest.assign(terms.begin() + g * w, terms.end());
			I32 finish = 2 * (I32)(w / 2) + 1; // w / 2 shuffle and op rounds, then the extract
			s.profit = (I32)n - 1 - (I32)s.rest.size() - ((I32)g - 1 + finish);
			return pack(s);
		}
	} // namespace slp
} // namespace rat
