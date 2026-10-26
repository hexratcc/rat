#include "pass/opt/load_canon.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"
#include "target/target.h"

namespace rat {
	LoadCanonPass::Handoff::Handoff(Function& fn)
	: fn(&fn),
		chains(collectChains(fn)),
		addrs(AliasAnalysis::restrictFacts(fn), fn.idBound()) {}

	List<SlpPackPass::Chain> LoadCanonPass::collectChains(Function& fn) {
		List<SlpPackPass::Chain> chains;
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

	// prev's only user besides loads, when it is a non-volatile store under prev's control
	StoreNode* LoadCanonPass::chainSuccessor(const StoreNode* prev) {
		StoreNode* next = nullptr;
		for(Node* u : prev->getUsers()) {
			if(isa<LoadNode>(u))
				continue;
			StoreNode* st = dyn_cast<StoreNode>(u);
			if(!st || next || st->getControl() != prev->getControl() || st->isVolatile())
				return nullptr;
			next = st;
		}
		return next;
	}

	// a reader of the state after store k, not the last, reads the state after store target < k,
	// the later of its last possible overlap and the latest state its cone reads
	void LoadCanonPass::hoistLoads(Handoff& h, U32 ptrBytes, U32 chainIndex, List<U32>& stateOf) {
		const SlpPackPass::Chain& chain = h.chains[chainIndex];
		Node* block = chain[0]->getControl();
		Conflicts seen;
		for(U32 k = 1; k <= chain.size(); ++k)
			stateOf[chain[k - 1]->getId()] = k;
		for(U32 k = 1; k < chain.size(); ++k) { // a hoist of a final state reader only adds an order
			seen.add(k, SlpPackPass::accessOf(h.addrs, ptrBytes, chain[k - 1]));
			SmallList<LoadNode*, 8> readers; // copy first, a hoist changes the store's users
			for(Node* u : chain[k - 1]->getUsers())
				if(LoadNode* l = dyn_cast<LoadNode>(u); l && l->getControl() == block && !l->isVolatile())
					readers.push_back(l);
			for(LoadNode* l : readers) {
				SlpPackPass::Access access = SlpPackPass::accessOf(h.addrs, ptrBytes, l);
				if(!access.addr.group)
					continue;
				U32 target = std::max(seen.lastConflict(access), coneState(l, stateOf));
				if(target >= k)
					continue;
				Node* state = chain[0]->getMemory(); // the state before the chain
				if(target)
					state = chain[target - 1];
				h.hoists.push_back({chainIndex, l, chain[k - 1]});
				l->setInput(1, state);
			}
		}
		for(StoreNode* st : chain)
			stateOf[st->getId()] = 0;
	}

	// the latest chain state read in l's address cone, ~0u past kMaxCone nodes
	U32 LoadCanonPass::coneState(LoadNode* l, const List<U32>& stateOf) {
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

	void LoadCanonPass::Conflicts::add(U32 store, const SlpPackPass::Access& a) {
		all.add(store, a.addr.group);
		if(!a.addr.object)
			lastWild = store;
		else
			objects[a.addr.object].add(store, a.addr.group);
		if(!a.addr.group)
			return;
		I64 end = a.addr.offset + (I64)a.size;
		for(I64 from = a.addr.offset; from < end;) { // each 16-byte block
			I64 to = SlpPackPass::blockEnd(from, end);
			std::array<U32, SlpPackPass::kVecBytes>& writers =
					blocks[SlpPackPass::pairKey(a.addr.group, from >> SlpPackPass::kVecShift)];
			for(I64 byte = from; byte < to; ++byte)
				writers[byte & (SlpPackPass::kVecBytes - 1)] = store;
			from = to;
		}
	}

	U32 LoadCanonPass::Conflicts::lastConflict(const SlpPackPass::Access& a) const {
		U32 conflict = lastWild;
		if(!a.addr.object)
			conflict = all.outside(a.addr.group);
		else if(auto it = objects.find(a.addr.object); it != objects.end())
			conflict = std::max(conflict, it->second.outside(a.addr.group));
		I64 end = a.addr.offset + (I64)a.size;
		for(I64 from = a.addr.offset; from < end;) { // each 16-byte block
			I64 to = SlpPackPass::blockEnd(from, end);
			auto it = blocks.find(SlpPackPass::pairKey(a.addr.group, from >> SlpPackPass::kVecShift));
			for(I64 byte = from; it != blocks.end() && byte < to; ++byte)
				conflict = std::max(conflict, it->second[byte & (SlpPackPass::kVecBytes - 1)]);
			from = to;
		}
		return conflict;
	}

	void LoadCanonPass::LastOutside::add(U32 store, U32 group) {
		if(group != lastGroup)
			lastOther = last;
		lastGroup = group;
		last = store;
	}

	U32 LoadCanonPass::LastOutside::outside(U32 group) const {
		if(group != lastGroup)
			return last;
		return lastOther;
	}

	LoadCanonPass::Handoff& LoadCanonPass::handoff() {
		thread_local Handoff h;
		return h;
	}

	LoadCanonPass::Handoff LoadCanonPass::takeHandoff(Function& fn) {
		Handoff& h = handoff();
		if(h.fn == &fn && h.version == fn.getVersion()) {
			h.fn = nullptr; // taken
			return std::move(h);
		}
		return Handoff(fn);
	}

	// a chain that did not pack gets its loads back, else they could load long before their use
	void LoadCanonPass::unhoistLoads(const List<Hoist>& hoists, const List<B32>& packed) {
		for(const Hoist& hoist : hoists)
			if(!packed[hoist.chain])
				hoist.load->setInput(1, hoist.state);
	}

	const C8* LoadCanonPass::name() const { return "loadcanon"; }

	U32 LoadCanonPass::runOnFunction(Function& fn, const TargetInfo& target) {
		Handoff& h = handoff();
		h = Handoff(fn);
		List<U32> stateOf; // node id -> store number in the walked chain, sized on first use
		for(U32 c = 0; c < h.chains.size(); ++c)
			if(SlpPackPass::hasInnerReader(h.chains[c].data(), (U32)h.chains[c].size())) {
				stateOf.resize(fn.idBound(), 0);
				hoistLoads(h, target.getPointerSizeInBytes(), c, stateOf);
			}
		h.version = fn.getVersion();
		return (U32)h.hoists.size();
	}
} // namespace rat
