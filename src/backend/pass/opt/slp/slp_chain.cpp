#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

#include <cstdlib>

namespace rat {
	namespace slp {

		Access accessOf(AddrCache& addrs, U32 ptrBytes, Node* n) {
			Type* type = n->getType();
			if(StoreNode* st = dyn_cast<StoreNode>(n))
				type = st->getValue()->getType();
			Access access{addrs.get(n->getInput(2)), type->byteSize(ptrBytes)}; // input 2 is the pointer
			if(!access.size || access.size > kVecBytes || access.addr.offset > kMaxOffset ||
				 access.addr.offset < -kMaxOffset)
				access.addr = Addr{};
			return access;
		}

		StoreNode* chainSuccessor(const StoreNode* prev) {
			StoreNode* next = nullptr;
			for(Node* u : prev->getUsers()) {
				StoreNode* st = dyn_cast<StoreNode>(u);
				if(isa<LoadNode>(u))
					continue;
				if(!st || next || st->getControl() != prev->getControl() || st->isVolatile())
					return nullptr;
				next = st;
			}
			return next;
		}

		List<Chain> collectChains(Function& fn) {
			List<Chain> chains;
			for(Node* n : fn) {
				StoreNode* st = dyn_cast<StoreNode>(n);
				if(!st || st->isVolatile())
					continue;
				StoreNode* prev = dyn_cast<StoreNode>(st->getMemory());
				if(prev && !prev->isVolatile() && chainSuccessor(prev) == st)
					continue; // not a chain head
				chains.emplace_back();
				for(StoreNode* store = st; store; store = chainSuccessor(store))
					chains.back().push_back(store);
			}
			return chains;
		}

		B32 isDisjointSet(const List<Access>& accesses, U32 first, U32 last, Map<U64, U16>& written) {
			List<Addr> groups; // address of each group's first store
			written.clear();
			for(U32 i = first; i <= last; ++i) {
				const Addr& a = accesses[i].addr;
				if(!a.group)
					return false;
				B32 seen = false;
				for(const Addr& g : groups) {
					if(g.group == a.group)
						seen = true;
					else if(!disjointBases(g, a))
						return false; // two groups may overlap
				}
				if(!seen)
					groups.push_back(a);
				if(groups.size() > kMaxSetGroups)
					return false;
				I64 end = a.offset + (I64)accesses[i].size;
				for(I64 from = a.offset, to = 0; from < end; from = to) { // each 16-byte block
					to = std::min(end, (from | (I64)(kVecBytes - 1)) + 1);
					U16 bits = (U16)(((1u << (to - from)) - 1) << (from & (kVecBytes - 1)));
					U16& mask = written[pairKey(a.group, from >> kVecShift)];
					if(mask & bits)
						return false; // two stores share a byte
					mask |= bits;
				}
			}
			return true;
		}

		MemMergeNode* Slp::mergeStretch(const Chain& chain, U32 first, U32 last) {
			Node* before = chain[first]->getMemory();
			StoreNode* lastStore = chain[last];
			List<Node*> inputs = {lastStore->getControl()};
			inputs.insert(inputs.end(), chain.begin() + first, chain.begin() + last + 1);
			MemMergeNode* set = fn.create<MemMergeNode>(fn.memTy(), inputs);
			lastStore->replaceAllUsesWith(set);					// the readers of the final state
			set->setInput(last - first + 1, lastStore); // replaceAllUsesWith also rewired set's own input
			for(U32 i = first + 1; i <= last; ++i)
				chain[i]->setInput(1, before);
			return set;
		}

		// a disjoint stretch becomes a set, the sets and the parts between are searched in chain order
		U32 Slp::packChains(const List<Chain>& chains, List<B32>& packed) {
			U32 changed = 0;
			packed.reserve(chains.size());
			List<Access> accesses;
			Map<U64, U16> written;
			for(const Chain& chain : chains) {
				U32 before = changed;
				accesses.clear();
				for(StoreNode* st : chain)
					accesses.push_back(accessOf(addrs, ptrBytes, st));
				U32 partStart = 0; // first store of the part not searched yet
				for(U32 first = 0, last = 0; first < chain.size(); first = last + 1) {
					for(last = first; last + 1 < chain.size() && chain[last]->getUsers().size() == 1;)
						++last;
					if(last == first || !isDisjointSet(accesses, first, last, written))
						continue;
					changed += packChain(Chain(chain.begin() + partStart, chain.begin() + first), nullptr);
					MemMergeNode* set = mergeStretch(chain, first, last);
					changed += packSet(set);
					lowerSet(set); // later parts and chains walk its stores as plain stores
					partStart = last + 1;
				}
				changed += packChain(Chain(chain.begin() + partStart, chain.end()), nullptr);
				packed.push_back(changed != before);
			}
			return changed;
		}

		// a chain without a pack gets its loads back on their old states, as an earlier state only
		// lets the scheduler load a value long before its use
		void unhoistLoads(const List<Hoist>& hoists, const List<B32>& packed) {
			for(const Hoist& hoist : hoists)
				if(!packed[hoist.chain])
					hoist.load->setInput(1, hoist.state); // input 1 is the memory
		}

		void lowerSet(MemMergeNode* set) {
			Node* prev = nullptr;
			for(U32 i = 1; i < set->getInputCount(); ++i)
				if(Node* st = set->getInput(i)) { // a slot a pack emptied is null
					if(prev)
						st->setInput(1, prev);
					prev = st;
				}
			set->replaceAllUsesWith(prev);
			set->clearInputs();
		}

		// bytes / element size stores from chain[start] of one type and group that fill one span
		B32 Slp::windowAt(const Chain& chain, U32 start, U32 bytes, Window& window) {
			if(start >= chain.size())
				return false;
			Type* type = chain[start]->getValue()->getType();
			if(!(typeBits(type) & kElem))
				return false;
			U32 elemSize = type->byteSize(ptrBytes);
			U32 laneCount = bytes / elemSize;
			if(laneCount < 2 || start + laneCount > chain.size())
				return false;
			const Addr& firstAddr = addrs.get(chain[start]->getPointer());
			I64 lowest = firstAddr.offset;
			I64 offsets[kMaxLanes];
			for(U32 j = 0; j < laneCount; ++j) {
				const Addr& a = addrs.get(chain[start + j]->getPointer());
				if(chain[start + j]->getValue()->getType() != type || a.group != firstAddr.group)
					return false;
				offsets[j] = a.offset;
				lowest = std::min(lowest, a.offset);
			}
			window = Window{start, laneCount, firstAddr.group, lowest};
			for(U32 j = 0; j < laneCount; ++j) { // the offsets fill the lanes above lowest once each
				U64 delta = (U64)(offsets[j] - lowest);
				U64 lane = delta / elemSize;
				if(delta % elemSize != 0 || lane >= laneCount || window.laneStore[lane])
					return false;
				window.laneStore[lane] = chain[start + j];
			}
			return true;
		}

		// windows along a chain, or along a run of a set at consecutive offsets
		U32 Slp::packChain(const Chain& chain, MemMergeNode* set) {
			U32 changed = 0;
			for(U32 i = 0; i < chain.size();) {
				U32 packedStores = packRunAt(chain, i, kVecBytes, set);
				if(!packedStores)
					packedStores = packRunAt(chain, i, kVecBytes / 2, set); // 2 x 32-bit
				changed += packedStores != 0;
				i += std::max(packedStores, 1u);
			}
			return changed;
		}

		U32 Slp::packSet(MemMergeNode* set) {
			Map<U64, Pair<StoreNode*, Type*>> members; // (group, offset) -> member and its value type
			for(U32 i = 1; i < set->getInputCount(); ++i) {
				StoreNode* st = cast<StoreNode>(set->getInput(i));
				const Addr& addr = addrs.get(st->getPointer());
				members[pairKey(addr.group, addr.offset)] = {st, st->getValue()->getType()};
			}
			U32 changed = 0;
			Chain run;
			for(U32 i = 1; i < set->getInputCount(); ++i) {
				StoreNode* st = dyn_cast<StoreNode>(set->getInput(i));
				if(!st || !(typeBits(st->getValue()->getType()) & kElem))
					continue; // a slot a pack emptied, or a narrow or vector store
				Type* type = st->getValue()->getType();
				I64 elemSize = type->byteSize(ptrBytes);
				const Addr& addr = addrs.get(st->getPointer());
				auto below = members.find(pairKey(addr.group, addr.offset - elemSize));
				if(below != members.end() && below->second.second == type)
					continue; // not the first of its run
				run.clear();
				for(I64 offset = addr.offset;; offset += elemSize) {
					auto it = members.find(pairKey(addr.group, offset));
					if(it == members.end() || it->second.second != type)
						break; // a gap or another type ends the run
					run.push_back(it->second.first);
				}
				changed += packChain(run, set);
			}
			return changed;
		}

		U32 Slp::packRunAt(const Chain& chain, U32 start, U32 bytes, MemMergeNode* set) {
			Window first;
			if(!windowAt(chain, start, bytes, first))
				return 0;
			++stats.windowsSeen;
			B32 observed = false;
			for(U32 j = 0; j + 1 < first.laneCount; ++j)
				observed |= chain[start + j]->getUsers().size() > 1; // a load besides the next store
			static const B32 noGuards = std::getenv("RAT_SLP_NO_GUARDS") != nullptr;
			if(observed && noGuards) {
				++stats.rejected;
				return 0;
			}
			Seed seed;
			seed.set = set;
			addWindow(seed, chain, first);
			if(observed)
				extendRun(chain, first, bytes, seed);
			if(!pack(seed))
				return 0;
			return (U32)seed.stores.size();
		}

		// add to seed the windows after first of its group and width that continue its bytes
		void Slp::extendRun(const Chain& chain, const Window& first, U32 bytes, Seed& seed) {
			Window next;
			for(U32 windows = 1; windows < kMaxRunWindows; ++windows) {
				if(!windowAt(chain, first.start + windows * first.laneCount, bytes, next))
					break;
				B32 continues = next.group == first.group && next.laneCount == first.laneCount &&
												next.offset == first.offset + (I64)(windows * bytes);
				if(!continues)
					break;
				addWindow(seed, chain, next);
				++stats.windowsSeen;
			}
		}

		// add the stores of window, its tuple of values and its profit to seed
		void addWindow(Seed& seed, const Chain& chain, const Window& window) {
			for(U32 j = 0; j < window.laneCount; ++j)
				seed.stores.push_back(chain[window.start + j]);
			seed.lowestStores.push_back(window.laneStore[0]);
			seed.tuples.emplace_back();
			for(U32 lane = 0; lane < window.laneCount; ++lane)
				seed.tuples.back().push_back(window.laneStore[lane]->getValue());
			seed.profit += (I32)window.laneCount - 1 - kMinProfit; // the fused store, less the minimum
		}

		// a reducible op with an input of its op and no user of its op and type
		B32 Slp::isReductionRoot(const Node* n) {
			Opcode op = n->getOpcode();
			if(op != Opcode::Add && op != Opcode::Mul && (op < Opcode::And || op > Opcode::Xor))
				return false;
			U8 typeClass = typeBits(n->getType());
			if(!(typeClass & kI32I64) || (op == Opcode::Mul && (typeClass != kI32 || !sse41)))
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
				changed += packReduction(root);
			return changed;
		}

		List<Node*> flattenReduction(Node* root, List<Node*>& steps) {
			List<Node*> terms;
			List<Node*> todo = {root};
			while(!todo.empty()) {
				Node* n = todo.back();
				todo.pop_back();
				B32 isStep = n->getOpcode() == root->getOpcode() && n->getType() == root->getType() &&
										 (n == root || n->getUsers().size() == 1);
				// a step replaces n by its two inputs, which can each end as a term
				if(!isStep || terms.size() + todo.size() + 2 > kMaxTerms) {
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

		U32 Slp::packReduction(Node* root) {
			Seed seed;
			seed.root = root;
			U32 laneCount = kVecBytes / root->getType()->byteSize(ptrBytes);
			List<Node*> terms = flattenReduction(root, seed.steps);
			U32 termCount = (U32)terms.size();
			U32 vectorCount = termCount / laneCount;
			if(termCount < kMinTerms)
				return 0;
			++stats.windowsSeen;
			seed.tuples.resize(vectorCount);
			for(U32 i = 0; i < vectorCount; ++i)
				seed.tuples[i].assign(terms.data() + i * laneCount, terms.data() + (i + 1) * laneCount);
			seed.scalarTerms.assign(terms.begin() + vectorCount * laneCount, terms.end());
			I32 finishOps = 2 * (I32)(laneCount / 2) + 1; // shuffle and op per round, then the extract
			seed.profit = (I32)termCount - 1 - (I32)seed.scalarTerms.size() -
										((I32)vectorCount - 1 + finishOps) - kMinProfit;
			return pack(seed);
		}
	} // namespace slp
} // namespace rat
