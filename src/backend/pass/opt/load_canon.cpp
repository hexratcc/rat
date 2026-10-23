#include "pass/opt/load_canon.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"
#include "target/target.h"

namespace rat {
	namespace canon {
		void LastOutside::add(U32 at, U32 group) {
			if(group != lastGroup)
				lastOther = last;
			lastGroup = group;
			last = at;
		}

		U32 LastOutside::outside(U32 group) const {
			if(group != lastGroup)
				return last;
			return lastOther;
		}

		// offset >> kVecShift is the 16-byte block, rounded down also for a negative offset
		U64 Conflicts::blockKey(U32 group, I64 offset) {
			return (U64)group << 32 | (U32)(offset >> kVecShift);
		}

		void Conflicts::add(U32 at, const Access& a) {
			all.add(at, a.addr.group);
			if(!a.addr.object)
				lastWild = at;
			else
				objects[a.addr.object].add(at, a.addr.group);
			I64 end = a.addr.offset + (I64)a.size;
			for(I64 o = a.addr.offset, stop = 0; a.addr.group && o < end; o = stop) { // each block
				stop = std::min(end, (o | (I64)(kVecBytes - 1)) + 1);
				std::array<U32, kVecBytes>& last = blocks[blockKey(a.addr.group, o)];
				for(I64 b = o; b < stop; ++b)
					last[b & (kVecBytes - 1)] = at;
			}
		}

		U32 Conflicts::lastConflict(const Access& a) const {
			U32 t = 0;
			if(!a.addr.object)
				t = all.outside(a.addr.group);
			else if(auto it = objects.find(a.addr.object); it != objects.end())
				t = std::max(lastWild, it->second.outside(a.addr.group));
			else
				t = lastWild;
			I64 end = a.addr.offset + (I64)a.size;
			for(I64 o = a.addr.offset, stop = 0; o < end; o = stop) { // each block
				stop = std::min(end, (o | (I64)(kVecBytes - 1)) + 1);
				auto it = blocks.find(blockKey(a.addr.group, o));
				for(I64 b = o; it != blocks.end() && b < stop; ++b)
					t = std::max(t, it->second[b & (kVecBytes - 1)]);
			}
			return t;
		}

		// the latest chain state l's address depends on, as an index, ~0 past kMaxCone nodes
		U32 coneState(LoadNode* l, const List<U32>& index) {
			U32 top = 0, visits = 0;
			SmallList<const Node*, 16> work;
			work.push_back(l->getPointer());
			while(!work.empty()) {
				const Node* c = work.back();
				work.pop_back();
				if(++visits > kMaxCone)
					return ~0u;
				U32 at = 0; // the chain index of the state c reads, 0 outside the chain
				if(Node* m = slp::memoryOperand(c))
					at = index[m->getId()];
				top = std::max(top, at);
				if(at || isa<PhiNode>(c))
					continue;
				for(U32 i = 0; i < c->getInputCount(); ++i)
					if(const Node* in = c->getInput(i); in && in->getType() && in->getType()->isData())
						work.push_back(in);
			}
			return top;
		}

		// some load reads an inner state of a chain, so hoistLoads may move it
		B32 readsInner(const List<Chain>& chains) {
			for(const Chain& chain : chains)
				for(U32 k = 0; k + 1 < chain.size(); ++k)
					for(Node* u : chain[k]->getUsers())
						if(isa<LoadNode>(u))
							return true;
			return false;
		}

		// move each load of the block that reads a chain state to the earliest chain state that holds
		// its bytes, the number of moved loads
		U32 hoistLoads(AddrCache& addrs, U32 ptrBytes, const Chain& chain, List<U32>& index) {
			Node* block = chain[0]->getControl();
			Conflicts seen;
			SmallList<LoadNode*, 8> readers;
			U32 moved = 0;
			for(U32 k = 1; k <= chain.size(); ++k)
				index[chain[k - 1]->getId()] = k;
			for(U32 k = 1; k < chain.size(); ++k) { // a hoist of a final state reader only adds an order
				seen.add(k, slp::accessOf(addrs, ptrBytes, chain[k - 1]));
				readers.clear(); // copy first, a hoist changes the store's users
				for(Node* u : chain[k - 1]->getUsers())
					if(LoadNode* l = dyn_cast<LoadNode>(u); l && l->getControl() == block && !l->isVolatile())
						readers.push_back(l);
				for(LoadNode* l : readers) {
					Access la = slp::accessOf(addrs, ptrBytes, l);
					if(!la.addr.group)
						continue;
					U32 t = std::max(seen.lastConflict(la), coneState(l, index));
					if(t >= k)
						continue;
					Node* state = chain[0]->getMemory(); // the state before the chain
					if(t)
						state = chain[t - 1];
					l->setInput(1, state); // input 1 is the memory
					++moved;
				}
			}
			for(StoreNode* st : chain)
				index[st->getId()] = 0;
			return moved;
		}
	} // namespace canon

	const C8* LoadCanonPass::name() const { return "loadcanon"; }

	U32 LoadCanonPass::runOnFunction(Function& fn, const TargetInfo& target) {
		slp::Handoff& h = slp::handoff();
		h.chains = slp::collectChains(fn);
		h.addrs.reset();
		U32 moved = 0;
		if(canon::readsInner(h.chains)) {
			h.addrs = std::make_unique<AddrCache>(AliasAnalysis::restrictFacts(fn), fn.idBound());
			List<U32> index(fn.idBound(), 0); // node id -> index from 1 in the chain being walked
			for(const canon::Chain& chain : h.chains)
				moved += canon::hoistLoads(*h.addrs, target.getPointerSizeInBytes(), chain, index);
		}
		h.fn = &fn;
		h.version = fn.getVersion();
		return moved;
	}
} // namespace rat
