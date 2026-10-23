#include "pass/opt/load_canon.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"
#include "target/target.h"

namespace rat {
	namespace slp {
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

		void Conflicts::add(U32 at, const Access& a) {
			all.add(at, a.addr.group);
			if(!a.addr.object)
				lastWild = at;
			else
				objects[a.addr.object].add(at, a.addr.group);
			I64 end = a.addr.offset + (I64)a.size;
			for(I64 o = a.addr.offset, stop = 0; a.addr.group && o < end; o = stop) { // each block
				stop = std::min(end, (o | (I64)(kVecBytes - 1)) + 1);
				std::array<U32, kVecBytes>& last = blocks[pairKey(a.addr.group, o >> kVecShift)];
				for(I64 b = o; b < stop; ++b)
					last[b & (kVecBytes - 1)] = at;
			}
		}

		U32 Conflicts::lastConflict(const Access& a) const {
			U32 t = lastWild;
			if(!a.addr.object)
				t = all.outside(a.addr.group);
			else if(auto it = objects.find(a.addr.object); it != objects.end())
				t = std::max(t, it->second.outside(a.addr.group));
			I64 end = a.addr.offset + (I64)a.size;
			for(I64 o = a.addr.offset, stop = 0; o < end; o = stop) { // each block
				stop = std::min(end, (o | (I64)(kVecBytes - 1)) + 1);
				auto it = blocks.find(pairKey(a.addr.group, o >> kVecShift));
				for(I64 b = o; it != blocks.end() && b < stop; ++b)
					t = std::max(t, it->second[b & (kVecBytes - 1)]);
			}
			return t;
		}

		U32 coneState(LoadNode* l, const List<U32>& index) {
			U32 top = 0, visits = 0;
			SmallList<const Node*, 16> work;
			work.push_back(l->getPointer());
			while(!work.empty()) {
				const Node* c = work.back();
				work.pop_back();
				if(++visits > kMaxCone)
					return ~0u;
				U32 at = 0;
				if(Node* m = memoryInput(c))
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

		B32 readsInner(const List<Chain>& chains) {
			for(const Chain& chain : chains)
				for(U32 k = 0; k + 1 < chain.size(); ++k)
					for(Node* u : chain[k]->getUsers())
						if(isa<LoadNode>(u))
							return true;
			return false;
		}

		U32 hoistLoads(AddrCache& addrs, U32 ptrBytes, const Chain& chain, List<U32>& index) {
			Node* block = chain[0]->getControl();
			Conflicts seen;
			SmallList<LoadNode*, 8> readers;
			U32 moved = 0;
			for(U32 k = 1; k <= chain.size(); ++k)
				index[chain[k - 1]->getId()] = k;
			for(U32 k = 1; k < chain.size(); ++k) { // a hoist of a final state reader only adds an order
				seen.add(k, accessOf(addrs, ptrBytes, chain[k - 1]));
				readers.clear(); // copy first, a hoist changes the store's users
				for(Node* u : chain[k - 1]->getUsers())
					if(LoadNode* l = dyn_cast<LoadNode>(u); l && l->getControl() == block && !l->isVolatile())
						readers.push_back(l);
				for(LoadNode* l : readers) {
					Access la = accessOf(addrs, ptrBytes, l);
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
	} // namespace slp

	const C8* LoadCanonPass::name() const { return "loadcanon"; }

	U32 LoadCanonPass::runOnFunction(Function& fn, const TargetInfo& target) {
		slp::Handoff& h = slp::handoff();
		h = {&fn, 0, slp::collectChains(fn), AddrCache(AliasAnalysis::restrictFacts(fn), fn.idBound())};
		U32 moved = 0;
		if(slp::readsInner(h.chains)) {
			List<U32> index(fn.idBound(), 0); // node id -> index from 1 in the chain being walked
			for(const slp::Chain& chain : h.chains)
				moved += slp::hoistLoads(h.addrs, target.getPointerSizeInBytes(), chain, index);
		}
		h.version = fn.getVersion();
		return moved;
	}
} // namespace rat
