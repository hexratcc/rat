#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		B32 SortKey::operator<(const SortKey& o) const {
			return std::tie(groupAt, offset, at) < std::tie(o.groupAt, o.offset, o.at);
		}

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

		StoreNode* chainSuccessor(StoreNode* prev) {
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

		// sort each stretch of stores whose inner states no load reads
		void Slp::orderChain(Chain& chain) {
			List<Access> acc;
			for(StoreNode* st : chain)
				acc.push_back(accessOf(addrs, ptrBytes, st));
			for(U32 a = 0, b = 0; a < chain.size(); a = b + 1) {
				for(b = a; b + 1 < chain.size() && chain[b]->getUsers().size() == 1;)
					++b;
				if(b > a)
					sortStretch(chain, acc, a, b);
			}
		}

		// sort chain[lo..hi] by group (first seen first) and offset when the stores that swap are disjoint
		void sortStretch(Chain& chain, const List<Access>& acc, U32 lo, U32 hi) {
			Map<U32, U32> first; // group -> first index
			List<SortKey> keys;
			for(U32 i = lo; i <= hi; ++i) {
				if(!acc[i].addr.group)
					return;
				U32 groupAt = first.try_emplace(acc[i].addr.group, i).first->second;
				keys.push_back({groupAt, acc[i].addr.offset, i});
			}
			if(first.size() > kMaxSortGroups || std::is_sorted(keys.begin(), keys.end()))
				return;
			std::sort(keys.begin(), keys.end());
			if(!sortDisjoint(acc, keys))
				return;
			Chain old(chain.begin() + lo, chain.begin() + hi + 1);
			Node* prev = old[0]->getMemory();
			for(U32 p = 0; p < keys.size(); ++p)
				chain[lo + p] = old[keys[p].at - lo];
			replaceUses(old.back(), chain[hi]); // readers of the final state
			for(U32 p = lo; p <= hi; ++p) {
				chain[p]->setInput(1, prev);
				prev = chain[p];
			}
		}

		B32 sortDisjoint(const List<Access>& acc, const List<SortKey>& keys) {
			List<U32> firsts; // keys are sorted by group first index
			for(const SortKey& k : keys)
				if(firsts.empty() || firsts.back() != k.groupAt)
					firsts.push_back(k.groupAt);
			for(U32 a = 0; a < firsts.size(); ++a)
				for(U32 b = a + 1; b < firsts.size(); ++b)
					if(!disjointBases(acc[firsts[a]].addr, acc[firsts[b]].addr))
						return false;
			for(U32 p = 1; p < keys.size(); ++p) {
				const Access &x = acc[keys[p - 1].at], &y = acc[keys[p].at];
				if(keys[p - 1].groupAt == keys[p].groupAt && !disjoint(x.addr, x.size, y.addr, y.size))
					return false;
			}
			return true;
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
