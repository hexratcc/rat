#include "pass/opt/slp/slp_pack_detail.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {
		// a disjoint stretch becomes a set, the sets and the parts between are searched in chain order
		U32 Slp::packChains(const List<Chain>& chains, List<B32>& packed) {
			U32 changed = 0;
			packed.reserve(chains.size());
			Map<U64, U16> written;
			for(const Chain& chain : chains) {
				U32 before = changed;
				U32 partStart = 0; // first store of the part not searched yet
				for(U32 first = 0; first < chain.size();) {
					U32 last = first; // each store before last has the next store as its only user
					while(last + 1 < chain.size() && chain[last]->getUsers().size() == 1)
						++last;
					if(last != first && isDisjointSet(chain, first, last, written)) {
						changed += packWindows(Chain(chain.begin() + partStart, chain.begin() + first));
						changed += packSet(chain, first, last);
						partStart = last + 1;
					}
					first = last + 1;
				}
				changed += packWindows(Chain(chain.begin() + partStart, chain.end()));
				packed.push_back(changed != before);
			}
			return changed;
		}

		// chain[first..last] share no byte, in at most kMaxSetGroups groups with disjoint bases (F3)
		B32 Slp::isDisjointSet(const Chain& chain, U32 first, U32 last, Map<U64, U16>& written) {
			List<Addr> groups; // address of each group's first store
			written.clear();
			for(U32 i = first; i <= last; ++i) {
				Access access = accessOf(cx.addrs, cx.ptrBytes, chain[i]);
				const Addr& a = access.addr;
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
				I64 end = a.offset + (I64)access.size;
				for(I64 from = a.offset; from < end;) { // each 16-byte block
					I64 to = blockEnd(from, end);
					U16 bits = (U16)(((1u << (to - from)) - 1) << (from & (kVecBytes - 1)));
					U16& mask = written[pairKey(a.group, from >> kVecShift)];
					if(mask & bits)
						return false; // two stores share a byte
					mask |= bits;
					from = to;
				}
			}
			return true;
		}

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

		// windows along a chain, or along consecutive stores of a set
		U32 Slp::packWindows(const Chain& chain) {
			U32 changed = 0;
			for(U32 i = 0; i < chain.size();) {
				U32 packedStores = packRunAt(chain, i, kVecBytes);
				if(!packedStores)
					packedStores = packRunAt(chain, i, kVecBytes / 2); // 2 x 32-bit
				changed += packedStores != 0;
				i += std::max(packedStores, 1u);
			}
			return changed;
		}

		// the stores packed by the run at chain[start], 0 when it does not pack
		U32 Slp::packRunAt(const Chain& chain, U32 start, U32 bytes) {
			Window first;
			if(!windowAt(chain, start, bytes, first))
				return 0;
			++stats.windowsSeen;
			B32 observed = hasInnerReader(chain.data() + start, first.laneCount);
			if(observed && cx.guards == SlpGuards::Off) {
				++stats.rejected;
				return 0;
			}
			Seed seed;
			addWindow(seed, chain, first);
			// an observed window takes the next windows that continue its bytes, under one guard
			Window next;
			for(U32 windows = 1; observed && windows < kMaxRunWindows; ++windows) {
				if(!windowAt(chain, start + windows * first.laneCount, bytes, next))
					break;
				B32 continues = next.group == first.group && next.laneCount == first.laneCount &&
												next.offset == first.offset + (I64)(windows * bytes);
				if(!continues)
					break;
				addWindow(seed, chain, next);
				++stats.windowsSeen;
			}
			if(!pack(seed))
				return 0;
			return (U32)seed.stores.size();
		}

		// bytes / element size stores from chain[start] of one type and group that fill one span
		B32 Slp::windowAt(const Chain& chain, U32 start, U32 bytes, Window& window) {
			if(start >= chain.size())
				return false;
			Type* type = chain[start]->getValue()->getType();
			if(!(typeBits(type) & Elem))
				return false;
			U32 elemSize = type->byteSize(cx.ptrBytes);
			U32 laneCount = bytes / elemSize;
			if(laneCount < 2 || start + laneCount > chain.size())
				return false;
			Addr firstAddr = cx.addrs.get(chain[start]->getPointer());
			I64 lowest = firstAddr.offset;
			I64 offsets[kMaxLanes];
			for(U32 j = 0; j < laneCount; ++j) {
				Addr a = cx.addrs.get(chain[start + j]->getPointer());
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

		// a load reads the state after one of stores[0..count-2]
		B32 hasInnerReader(StoreNode* const* stores, U32 count) {
			for(U32 k = 0; k + 1 < count; ++k)
				for(Node* u : stores[k]->getUsers())
					if(isa<LoadNode>(u))
						return true;
			return false;
		}

		void addWindow(Seed& seed, const Chain& chain, const Window& window) {
			for(U32 j = 0; j < window.laneCount; ++j)
				seed.stores.push_back(chain[window.start + j]);
			seed.lowestStores.push_back(window.laneStore[0]);
			seed.tuples.emplace_back();
			for(U32 lane = 0; lane < window.laneCount; ++lane)
				seed.tuples.back().push_back(window.laneStore[lane]->getValue());
			seed.profit += (I32)window.laneCount - 1; // the fused store
		}

		// chain[first..last] as a set, searched by offset, then lowered back to a chain
		U32 Slp::packSet(const Chain& chain, U32 first, U32 last) {
			MemMergeNode* set = mergeStretch(chain, first, last);
			Map<U64, Pair<StoreNode*, Type*>> members; // (group, offset) -> member and its value type
			for(U32 i = 1; i < set->getInputCount(); ++i) {
				StoreNode* st = cast<StoreNode>(set->getInput(i));
				Addr addr = cx.addrs.get(st->getPointer());
				members[pairKey(addr.group, addr.offset)] = {st, st->getValue()->getType()};
			}
			U32 changed = 0;
			Chain consecutive;
			for(U32 i = 1; i < set->getInputCount(); ++i) {
				StoreNode* st = dyn_cast<StoreNode>(set->getInput(i));
				if(!st || !(typeBits(st->getValue()->getType()) & Elem))
					continue; // a slot a pack emptied, or a narrow or vector store
				Type* type = st->getValue()->getType();
				I64 elemSize = type->byteSize(cx.ptrBytes);
				Addr addr = cx.addrs.get(st->getPointer());
				auto below = members.find(pairKey(addr.group, addr.offset - elemSize));
				if(below != members.end() && below->second.second == type)
					continue; // not the lowest of its consecutive stores
				consecutive.clear();
				for(I64 offset = addr.offset;; offset += elemSize) {
					auto it = members.find(pairKey(addr.group, offset));
					if(it == members.end() || it->second.second != type)
						break; // a gap or another type ends them
					consecutive.push_back(it->second.first);
				}
				changed += packWindows(consecutive);
			}
			lowerSet(set); // later parts and chains walk its stores as plain stores
			return changed;
		}

		// every store of the stretch reads the state before the stretch, a MemMerge joins them
		MemMergeNode* Slp::mergeStretch(const Chain& chain, U32 first, U32 last) {
			Node* before = chain[first]->getMemory();
			StoreNode* lastStore = chain[last];
			List<Node*> inputs = {lastStore->getControl()};
			inputs.insert(inputs.end(), chain.begin() + first, chain.begin() + last + 1);
			// made even when nothing packs, dropping it shifts the ids of later nodes
			MemMergeNode* set = cx.fn.create<MemMergeNode>(cx.fn.memTy(), inputs);
			lastStore->replaceAllUsesWith(set);					// the readers of the final state
			set->setInput(last - first + 1, lastStore); // replaceAllUsesWith also rewired set's own input
			for(U32 i = first + 1; i <= last; ++i)
				chain[i]->setInput(1, before);
			return set;
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

		// the state before a run, valid until commitRun clears the stores
		Node* memIn(const Seed& seed) { return seed.stores[0]->getMemory(); }

		// state is a run store but the last. Only runs ask, so stores is not empty
		B32 isInner(const Seed& seed, const Node* state) {
			return std::count(seed.stores.begin(), seed.stores.end() - 1, state) != 0;
		}

		// the set of a run of set members, their only user, or null for a chain run
		MemMergeNode* setOf(const Seed& seed) {
			return dyn_cast<MemMergeNode>(seed.stores[0]->getUsers()[0]);
		}
	} // namespace slp
} // namespace rat
