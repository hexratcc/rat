#include "pass/opt/load_canon.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"
#include "target/target.h"

namespace rat {
	namespace slp {
		void LastOutside::add(U32 store, U32 group) {
			if(group != lastGroup)
				lastOther = last;
			lastGroup = group;
			last = store;
		}

		U32 LastOutside::outside(U32 group) const {
			if(group != lastGroup)
				return last;
			return lastOther;
		}

		void Conflicts::add(U32 store, const Access& a) {
			all.add(store, a.addr.group);
			if(!a.addr.object)
				lastWild = store;
			else
				objects[a.addr.object].add(store, a.addr.group);
			I64 end = a.addr.offset + (I64)a.size;
			for(I64 from = a.addr.offset, to = 0; a.addr.group && from < end; from = to) { // each block
				to = std::min(end, (from | (I64)(kVecBytes - 1)) + 1);
				std::array<U32, kVecBytes>& writers = blocks[pairKey(a.addr.group, from >> kVecShift)];
				for(I64 byte = from; byte < to; ++byte)
					writers[byte & (kVecBytes - 1)] = store;
			}
		}

		U32 Conflicts::lastConflict(const Access& a) const {
			U32 conflict = lastWild;
			if(!a.addr.object)
				conflict = all.outside(a.addr.group);
			else if(auto it = objects.find(a.addr.object); it != objects.end())
				conflict = std::max(conflict, it->second.outside(a.addr.group));
			I64 end = a.addr.offset + (I64)a.size;
			for(I64 from = a.addr.offset, to = 0; from < end; from = to) { // each block
				to = std::min(end, (from | (I64)(kVecBytes - 1)) + 1);
				auto it = blocks.find(pairKey(a.addr.group, from >> kVecShift));
				for(I64 byte = from; it != blocks.end() && byte < to; ++byte)
					conflict = std::max(conflict, it->second[byte & (kVecBytes - 1)]);
			}
			return conflict;
		}

		// the latest chain state read in l's address cone, ~0u past kMaxCone nodes
		U32 coneState(LoadNode* l, const List<U32>& stateOf) {
			U32 latest = 0;
			U32 visits = 0;
			SmallList<const Node*, 16> work = {l->getPointer()};
			while(!work.empty()) {
				const Node* n = work.back();
				work.pop_back();
				if(++visits > kMaxCone)
					return ~0u;
				U32 state = 0;
				if(Node* mem = memoryInput(n))
					state = stateOf[mem->getId()];
				latest = std::max(latest, state);
				if(state || isa<PhiNode>(n))
					continue;
				for(U32 i = 0; i < n->getInputCount(); ++i)
					if(const Node* in = n->getInput(i); in && in->getType() && in->getType()->isData())
						work.push_back(in);
			}
			return latest;
		}

		// a load reads a state between the chain's stores
		B32 hasInnerReader(const Chain& chain) {
			for(U32 k = 0; k + 1 < chain.size(); ++k)
				for(Node* u : chain[k]->getUsers())
					if(isa<LoadNode>(u))
						return true;
			return false;
		}

		U32 hoistLoads(AddrCache& addrs, U32 ptrBytes, const Chain& chain, List<U32>& stateOf) {
			Node* block = chain[0]->getControl();
			Conflicts seen;
			U32 moved = 0;
			for(U32 k = 1; k <= chain.size(); ++k)
				stateOf[chain[k - 1]->getId()] = k;
			for(U32 k = 1; k < chain.size(); ++k) { // a hoist of a final state reader only adds an order
				seen.add(k, accessOf(addrs, ptrBytes, chain[k - 1]));
				SmallList<LoadNode*, 8> readers; // copy first, a hoist changes the store's users
				for(Node* u : chain[k - 1]->getUsers())
					if(LoadNode* l = dyn_cast<LoadNode>(u); l && l->getControl() == block && !l->isVolatile())
						readers.push_back(l);
				for(LoadNode* l : readers) {
					Access access = accessOf(addrs, ptrBytes, l);
					if(!access.addr.group)
						continue;
					U32 target = std::max(seen.lastConflict(access), coneState(l, stateOf));
					if(target >= k)
						continue;
					Node* state = chain[0]->getMemory(); // the state before the chain
					if(target)
						state = chain[target - 1];
					l->setInput(1, state); // input 1 is the memory
					++moved;
				}
			}
			for(StoreNode* st : chain)
				stateOf[st->getId()] = 0;
			return moved;
		}
	} // namespace slp

	const C8* LoadCanonPass::name() const { return "loadcanon"; }

	U32 LoadCanonPass::runOnFunction(Function& fn, const TargetInfo& target) {
		slp::Handoff& h = slp::handoff();
		h = {&fn, 0, slp::collectChains(fn), AddrCache(AliasAnalysis::restrictFacts(fn), fn.idBound())};
		U32 moved = 0;
		List<U32> stateOf; // node id -> store number in the walked chain, sized on first use
		for(const slp::Chain& chain : h.chains)
			if(slp::hasInnerReader(chain)) {
				stateOf.resize(fn.idBound(), 0);
				moved += slp::hoistLoads(h.addrs, target.getPointerSizeInBytes(), chain, stateOf);
			}
		h.version = fn.getVersion();
		return moved;
	}
} // namespace rat
