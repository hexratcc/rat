#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	U32 SlpPackPass::Plan::planLoad(const Lanes& lanes) {
		Vector vec{lanes, VectorKind::Load};
		if(!loadShape(lanes, vec) || !loadState(lanes, vec) || forwardBlocked(vec))
			return kNoVector;
		I32 laneCount = (I32)lanes.size();
		vec.profit = laneCount - 2; // one load and a broadcast
		if(vec.wideLoads)
			vec.profit = laneCount - (I32)vec.wideLoads - vec.permuted;
		vectors.push_back(vec);
		return (U32)vectors.size() - 1;
	}

	B32 SlpPackPass::Plan::loadShape(const Lanes& lanes, Vector& vec) {
		LoadNode* firstLoad = dyn_cast<LoadNode>(lanes[0]);
		if(!firstLoad)
			return false;
		U32 laneCount = lanes.size();
		U32 laneSize = firstLoad->getType()->byteSize(cx.ptrBytes);
		Addr firstAddr = cx.addrs.get(firstLoad->getPointer());
		// i8 and i16 lanes, the source of a convert, load only adjacent
		B32 elem = (typeBits(firstLoad->getType()) & Elem) != 0;
		B32 adjacent = true;
		B32 equal = elem;
		vec.end = firstAddr.offset + (I64)laneSize;
		vec.lowestLoad = firstLoad;
		vec.offset = firstAddr.offset;
		for(U32 i = 0; i < laneCount; ++i) {
			LoadNode* l = dyn_cast<LoadNode>(lanes[i]);
			if(!l || l->isVolatile() || l->getControl() != firstLoad->getControl())
				return false;
			Addr a = cx.addrs.get(l->getPointer());
			if(a.group != firstAddr.group)
				return false;
			adjacent &= a.offset == firstAddr.offset + (I64)(i * laneSize);
			equal &= a.offset == firstAddr.offset;
			if(a.offset < vec.offset)
				vec.lowestLoad = l;
			vec.offset = std::min(vec.offset, a.offset);
			vec.end = std::max(vec.end, a.offset + (I64)laneSize);
		}
		if(!equal) // else one load and a broadcast
			vec.wideLoads = 1 + (vec.end - vec.offset > (I64)(laneCount * laneSize));
		vec.permuted = !adjacent && !equal;
		return adjacent || equal || (elem && permuteSpan(lanes, vec));
	}

	// some load at ctrl reads each element no lane reads, so the wide loads add no fault
	B32 SlpPackPass::Plan::permuteSpan(const Lanes& lanes, Vector& vec) {
		U32 laneCount = lanes.size();
		U32 laneSize = vec.lowestLoad->getType()->byteSize(cx.ptrBytes);
		U32 group = cx.addrs.get(vec.lowestLoad->getPointer()).group;
		Node* ctrl = vec.lowestLoad->getControl();
		I64 bytes = (I64)(vec.wideLoads * laneCount * laneSize);
		// two loads must be full vectors, as the second starts kVecBytes higher
		if(vec.end - vec.offset > bytes || (vec.wideLoads == 2 && laneCount * laneSize != kVecBytes))
			return false;
		for(I64 start : {vec.offset, vec.end - bytes}) {
			U32 readElements = 0; // bit per element
			vec.sel = 0;
			for(U32 i = 0; i < laneCount; ++i) {
				I64 delta = cx.addrs.get(cast<LoadNode>(lanes[i])->getPointer()).offset - start;
				if(delta % laneSize)
					return false;
				readElements |= 1u << (delta / laneSize);
				vec.sel |= (U16)(delta / laneSize << (ShuffleNode::kSourceBits * i));
			}
			B32 covered = true;
			for(U32 element = 0; element < vec.wideLoads * laneCount; ++element)
				if(!(readElements >> element & 1))
					covered &= cx.bytesRead(group, start + (I64)(element * laneSize), laneSize, ctrl);
			if(covered) {
				vec.offset = start;
				return true;
			}
		}
		return false;
	}

	// some load at ctrl reads each 4-byte piece of [offset, offset + size) of group
	B32 SlpPackPass::Context::bytesRead(U32 group, I64 offset, U32 size, const Node* ctrl) {
		if(!readsIndexed)
			indexReads();
		auto it = reads.find(pairKey(ctrl->getId(), group));
		for(U32 piece = 0; piece < size; piece += kReadPiece)
			if(it == reads.end() || !it->second.count(offset + (I64)piece))
				return false;
		return true;
	}

	void SlpPackPass::Context::indexReads() {
		readsIndexed = true;
		for(Node* n : fn) {
			LoadNode* load = dyn_cast<LoadNode>(n);
			if(!load || load->isVolatile())
				continue;
			U32 size = load->getType()->byteSize(ptrBytes);
			if(!size || size % kReadPiece != 0)
				continue;
			Addr addr = addrs.get(load->getPointer());
			Set<I64>& pieces = reads[pairKey(load->getControl()->getId(), addr.group)];
			for(U32 piece = 0; piece < size; piece += kReadPiece)
				pieces.insert(addr.offset + (I64)piece);
		}
	}

	// the state the new loads read: the lanes' shared state, or memIn when each lane may read it
	B32 SlpPackPass::Plan::loadState(const Lanes& lanes, Vector& vec) {
		B32 shared = true;
		vec.mem = vec.lowestLoad->getMemory();
		for(Node* l : lanes)
			shared &= cast<LoadNode>(l)->getMemory() == vec.mem;
		if(seed.root)
			return shared; // a reduction has no memIn and adds no guard
		// in a block with a guarded run the new loads read memIn, as loads of an earlier state would
		// stay live across that guard
		if(shared && !cx.guardedRunsIn(vec.lowestLoad->getControl()) && !isInner(seed, vec.mem))
			return true;
		vec.mem = memIn(seed);
		Addr runAddr = cx.addrs.get(seed.lowestStores[0]->getPointer());
		for(Node* l : lanes)
			if(!canReadMemIn(cast<LoadNode>(l), runAddr, vec.needsGuard))
				return false;
		return true;
	}

	// l may read memIn when its edge to the run is none or weak, a weak edge sets needsGuard
	B32 SlpPackPass::Plan::canReadMemIn(LoadNode* l, const Addr& runAddr, B32& needsGuard) {
		Addr loadAddr = cx.addrs.get(l->getPointer());
		B32 inner = isInner(seed, l->getMemory());
		// only run stores lie between, all in the run's group, so the bases decide
		if(inner && loadAddr.group != runAddr.group) {
			needsGuard |= !disjointBases(loadAddr, runAddr);
			return true;
		}
		Node* later = memIn(seed);
		Node* earlier = l->getMemory();
		if(inner)
			std::swap(later, earlier);
		U32 size = l->getType()->byteSize(cx.ptrBytes);
		for(U32 hops = 0; later != earlier; ++hops) {
			StoreNode* st = dyn_cast<StoreNode>(later);
			if(!st || hops >= kMaxHops || st->isVolatile() || st->getControl() != l->getControl())
				return false;
			Addr storeAddr = cx.addrs.get(st->getPointer());
			if(!disjoint(loadAddr, size, storeAddr, st->getValue()->getType()->byteSize(cx.ptrBytes)))
				return false;
			later = st->getMemory();
		}
		return true;
	}

	// a wide load forwards only from one store with all its bytes, else stalls
	B32 SlpPackPass::Plan::forwardBlocked(const Vector& vec) {
		U32 bytes = (U32)vec.lanes.size() * vec.lowestLoad->getType()->byteSize(cx.ptrBytes);
		Access load{cx.addrs.get(vec.lowestLoad->getPointer()), bytes};
		for(U32 k = 0; k < vec.wideLoads; ++k) {
			load.addr.offset = vec.offset + (I64)(k * bytes);
			SmallList<Node*, kMaxHops> walk = {vec.mem};
			for(U32 i = 0; i < walk.size() && i < kMaxHops; ++i)
				if(splitsLoad(walk[i], load, walk))
					return true;
		}
		return false;
	}

	// state writes part of load or is a call that writes it, else walk takes the states before it
	B32 SlpPackPass::Plan::splitsLoad(Node* state,
																		const Access& load,
																		SmallList<Node*, kMaxHops>& walk) {
		if(PhiNode* phi = dyn_cast<PhiNode>(state)) {
			// only a loop header's entry, the walk cannot compare the previous iteration's addresses
			U32 count = phi->getValueCount();
			if(phi->getRegion()->isLoopHeader())
				count = 1;
			for(U32 i = 0; i < count; ++i)
				walk.push_back(phi->getValue(i));
			return false;
		}
		ProjNode* proj = dyn_cast<ProjNode>(state);
		if(proj && isa<CallNode>(proj->getProducer()))
			return callWrites(cast<CallNode>(proj->getProducer()), load.addr);
		StoreNode* st = dyn_cast<StoreNode>(state);
		if(!st)
			return false; // the entry state or another writer, taken as old
		Access store = accessOf(cx.addrs, cx.ptrBytes, st);
		if(disjoint(load.addr, load.size, store.addr, store.size)) {
			walk.push_back(st->getMemory());
			return false;
		}
		// a store of the group splits the load unless it holds all its bytes, a store of another
		// group that may overlap ends the walk
		I64 loadEnd = load.addr.offset + (I64)load.size;
		I64 storeEnd = store.addr.offset + (I64)store.size;
		B32 holdsAll = store.addr.offset <= load.addr.offset && storeEnd >= loadEnd;
		return store.addr.group == load.addr.group && !holdsAll;
	}

	// call takes the address of an object no store of fn writes, an out parameter or a struct return
	B32 SlpPackPass::Plan::callWrites(const CallNode* call, const Addr& addr) {
		if(!addr.object || cx.storesTo(addr.object))
			return false;
		for(U32 i = 0; i < call->getArgCount(); ++i) {
			Node* arg = call->getArg(i);
			if(arg->getType()->isPtr() && cx.addrs.get(arg).object == addr.object)
				return true;
		}
		return false;
	}

	B32 SlpPackPass::Context::storesTo(U32 object) {
		if(!storesIndexed)
			indexStores();
		return storedObjects.count(object) != 0;
	}

	void SlpPackPass::Context::indexStores() {
		storesIndexed = true;
		for(Node* n : fn) {
			StoreNode* st = dyn_cast<StoreNode>(n);
			if(st && st->getInputCount()) // a store commitRun packed has no inputs
				storedObjects.insert(addrs.get(st->getPointer()).object);
		}
	}
} // namespace rat
