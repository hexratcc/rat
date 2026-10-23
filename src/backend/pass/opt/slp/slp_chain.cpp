#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

#include <cstdlib>

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
					U16& mask = written[pairKey(a.group, o >> kVecShift)];
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

		// a disjoint stretch becomes a set, the sets and the parts between are searched in chain order
		U32 Slp::searchChains(const List<Chain>& chains, List<MemMergeNode*>& sets) {
			U32 changed = 0;
			List<Access> acc;
			Map<U64, U16> written;
			for(const Chain& chain : chains) {
				acc.clear();
				for(StoreNode* st : chain)
					acc.push_back(accessOf(addrs, ptrBytes, st));
				U32 from = 0; // first store of the part not searched yet
				for(U32 a = 0, b = 0; a < chain.size(); a = b + 1) {
					for(b = a; b + 1 < chain.size() && chain[b]->getUsers().size() == 1;)
						++b;
					if(b == a || !setDisjoint(acc, a, b, written))
						continue;
					if(from < a)
						changed += processChain(Chain(chain.begin() + from, chain.begin() + a), nullptr);
					MemMergeNode* m = mergeStretch(chain, a, b);
					changed += processSet(m);
					sets.push_back(m);
					from = b + 1;
				}
				if(from < chain.size())
					changed += processChain(Chain(chain.begin() + from, chain.end()), nullptr);
			}
			return changed;
		}

		void lowerMerge(MemMergeNode* m) {
			Node* prev = nullptr;
			for(U32 i = 1; i < m->getInputCount(); ++i) {
				Node* st = m->getInput(i);
				if(st && prev)
					st->setInput(1, prev);
				if(st)
					prev = st;
			}
			m->replaceAllUsesWith(prev);
			m->clearInputs();
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
			I64 lo = a0.offset, off[kMaxLanes];
			for(U32 j = 0; j < w; ++j) {
				const Addr& a = addrs.get(chain[at + j]->getPointer());
				if(chain[at + j]->getValue()->getType() != t || a.group != a0.group)
					return false;
				off[j] = a.offset;
				lo = std::min(lo, a.offset);
			}
			out = Window{at, w, bytes, a0.group, lo};
			for(U32 j = 0; j < w; ++j) { // the offsets fill the w lanes above lo once each
				U64 d = (U64)(off[j] - lo);
				if(d % esz != 0 || d / esz >= w || out.lane[d / esz])
					return false;
				out.lane[d / esz] = chain[at + j];
			}
			return true;
		}

		// windows along a chain, or along a run of a set at consecutive offsets
		U32 Slp::processChain(const Chain& chain, MemMergeNode* set) {
			U32 changed = 0;
			for(U32 i = 0; i < chain.size();) {
				U32 used = tryRun(chain, i, kVecBytes, set);
				if(!used)
					used = tryRun(chain, i, kVecBytes / 2, set); // 2 x 32-bit
				changed += used != 0;
				i += std::max(used, 1u);
			}
			return changed;
		}

		// windows of a set
		U32 Slp::processSet(MemMergeNode* m) {
			Map<U64, Pair<StoreNode*, Type*>> at; // (group, offset) -> member and its value type
			for(U32 i = 1; i < m->getInputCount(); ++i) {
				StoreNode* st = cast<StoreNode>(m->getInput(i));
				const Addr& a = addrs.get(st->getPointer());
				at[pairKey(a.group, a.offset)] = {st, st->getValue()->getType()};
				setSlot[st] = i;
			}
			U32 changed = 0;
			Chain run;
			for(U32 i = 1; i < m->getInputCount(); ++i) {
				StoreNode* st = dyn_cast<StoreNode>(m->getInput(i));
				if(!st || !(typeBits(st->getValue()->getType()) & kElem))
					continue; // a slot a pack emptied, or a narrow or vector store
				Type* t = st->getValue()->getType();
				I64 esz = t->byteSize(ptrBytes);
				const Addr& a = addrs.get(st->getPointer());
				auto prev = at.find(pairKey(a.group, a.offset - esz));
				if(prev != at.end() && prev->second.second == t)
					continue; // not the first of its run
				run.clear();
				for(auto it = at.find(pairKey(a.group, a.offset)); it != at.end() && it->second.second == t;
						it = at.find(pairKey(a.group, a.offset + (I64)run.size() * esz)))
					run.push_back(it->second.first);
				changed += processChain(run, m);
			}
			return changed;
		}

		U32 Slp::tryRun(const Chain& chain, U32 i, U32 bytes, MemMergeNode* set) {
			Window w0;
			if(!windowAt(chain, i, bytes, w0))
				return 0;
			++stats.windowsSeen;
			B32 observed = false;
			for(U32 j = 0; j + 1 < w0.w; ++j)
				observed |= chain[i + j]->getUsers().size() > 1; // a load besides the next store
			static const B32 noGuards = std::getenv("RAT_SLP_NO_GUARDS") != nullptr;
			if(observed && noGuards) {
				++stats.rejected;
				return 0;
			}
			Run run = {w0};
			if(observed)
				extendRun(chain, run);
			Seed seed = runSeed(chain, run, set);
			if(!pack(seed))
				return 0;
			return (U32)seed.stores.size();
		}

		// add the next windows of run[0]'s group and width that continue its bytes
		void Slp::extendRun(const Chain& chain, Run& run) {
			Window w0 = run[0], wn;
			while(run.size() < kMaxRunWindows) {
				if(!windowAt(chain, w0.begin + run.size() * w0.w, w0.bytes, wn))
					break;
				B32 continues =
						wn.group == w0.group && wn.w == w0.w && wn.lo == w0.lo + (I64)(run.size() * w0.bytes);
				if(!continues)
					break;
				run.push_back(wn);
				++stats.windowsSeen;
			}
		}

		// the run's stores and one tuple of lane values per window
		Seed runSeed(const Chain& chain, const Run& run, MemMergeNode* set) {
			Seed s;
			s.set = set;
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

		// a reducible op with an input of its op and no user of its op and type
		B32 Slp::isReductionRoot(const Node* n) {
			Opcode op = n->getOpcode();
			if(op != Opcode::Add && op != Opcode::Mul && (op < Opcode::And || op > Opcode::Xor))
				return false;
			U8 t = typeBits(n->getType());
			if(!(t & kI32I64) || (op == Opcode::Mul && (t != kI32 || !sse41)))
				return false;
			B32 root = false;
			for(U32 i = 0; i < n->getInputCount(); ++i)
				root |= n->getInput(i)->getOpcode() == op;
			for(Node* u : n->getUsers())
				root &= u->getOpcode() != op || u->getType() != n->getType();
			return root;
		}

		U32 Slp::packReductions() {
			List<Node*> roots;
			for(Node* n : fn)
				if(isReductionRoot(n))
					roots.push_back(n);
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
