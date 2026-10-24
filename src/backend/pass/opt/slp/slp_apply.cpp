#include "pass/opt/slp/slp_pack_detail.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		void Slp::apply(Plan& plan) {
			const Seed& seed = plan.seed;
			seedAnchors(plan);
			emitVectors(plan);
			if(seed.root) {
				commitReduction(plan);
				++stats.packedReduction;
			} else if(plan.guards.empty()) {
				commitRun(plan);
				stats.packedUnguarded += (U32)seed.lowestStores.size();
			} else {
				commitGuarded(plan);
				stats.packedGuarded += (U32)seed.lowestStores.size();
				++stats.guardedRuns;
				stats.guardChecks += (U32)plan.guards.size();
			}
		}

		// operands first, so an Op finds the nodes of its operands
		void Slp::emitVectors(Plan& plan) {
			for(Vector& vec : plan.vectors) {
				Type* vecTy = fn.types().getVec(laneType(vec.lanes[0]), vec.lanes.size());
				if(vec.kind == VectorKind::Load)
					vec.node = emitLoad(vec, vecTy, plan);
				else if(vec.kind == VectorKind::Op)
					vec.node = emitOp(vec, vecTy, plan);
				else
					vec.node = emitGather(vec, vecTy);
			}
		}

		// the node apply emitted for seed tuple
		Node* Plan::tupleNode(U32 tuple) const { return vectors[tupleVectors[tuple]].node; }

		Node* Slp::emitGather(const Vector& vec, Type* vecTy) {
			if(isSplat(vec.lanes))
				return fn.create<SplatNode>(vecTy, vec.lanes[0]);
			List<Node*> scalars(vec.lanes.begin(), vec.lanes.end());
			return fn.create<PackNode>(vecTy, scalars);
		}

		Node* Slp::emitLoad(const Vector& vec, Type* vecTy, Plan& plan) {
			if(!vec.wideLoads) {
				Node* load = loadAt(vec, vec.lowestLoad->getType(), vec.offset, plan);
				return fn.create<SplatNode>(vecTy, load);
			}
			Node* low = loadAt(vec, vecTy, vec.offset, plan);
			if(!vec.permuted)
				return low;
			Node* high = low;
			if(vec.wideLoads == 2)
				high = loadAt(vec, vecTy, vec.offset + (I64)kVecBytes, plan);
			return fn.create<ShuffleNode>(vecTy, low, high, vec.sel);
		}

		// a load of type ty at offset of vec's group from vec.mem, or an earlier vector's equal load
		Node* Slp::loadAt(const Vector& vec, Type* ty, I64 offset, Plan& plan) {
			Addr addr = addrs.get(vec.lowestLoad->getPointer());
			addr.offset = offset;
			for(const auto& [ld, ldAddr] : plan.loads)
				if(ld->getType() == ty && ld->getMemory() == vec.mem && ldAddr.group == addr.group &&
					 ldAddr.offset == offset && ld->getControl() == vec.lowestLoad->getControl())
					return ld;
			Node* ptr = anchorPtr(vec.lowestLoad->getPointer(), offset, vec.lowestLoad->getControl());
			LoadNode* ld = fn.create<LoadNode>(ty, vec.lowestLoad->getControl(), vec.mem, ptr);
			plan.loads.push_back({ld, addr});
			return ld;
		}

		// mainOp over the operand vectors, or mainOp and altOp blended by sel
		Node* Slp::emitOp(const Vector& vec, Type* vecTy, const Plan& plan) {
			Node* inputs[kMaxInputs] = {};
			for(U32 i = 0; i < kMaxInputs; ++i)
				if(vec.operands[i] != kNoVector)
					inputs[i] = plan.vectors[vec.operands[i]].node;
			if(vec.shiftAmount)
				inputs[1] = vec.shiftAmount;
			Node* result = makeOp(vec.mainOp, vecTy, inputs);
			if(vec.altOp == kNoOp)
				return result;
			return fn.create<ShuffleNode>(vecTy, result, makeOp(vec.altOp, vecTy, inputs), vec.sel);
		}

		Node* Slp::makeOp(Opcode op, Type* vecTy, Node* const* inputs) {
			switch(getOpClass(op)) {
			case OpClass::Binary:
				return fn.create<BinaryNode>(op, vecTy, inputs[0], inputs[1]);
			case OpClass::Unary:
				return fn.create<UnaryNode>(op, vecTy, inputs[0]);
			case OpClass::Compare:
				return fn.create<CompareNode>(op, vecTy, inputs[0], inputs[1]);
			case OpClass::Convert:
				return fn.create<ConvertNode>(op, vecTy, inputs[0]);
			default:
				return fn.create<SelectNode>(vecTy, inputs[0], inputs[1], inputs[2]);
			}
		}

		// a (control, group) the plan is the first to use takes the anchorOf base of the lowest offset
		// among the plan's pointers of it, the first in plan order on a tie. A group's equal addresses
		// can rest on nsw facts that hold only where its accesses run, so an anchor stays in its control
		void Slp::seedAnchors(const Plan& plan) {
			List<Pair<Node*, const Node*>> ptrs;
			for(const Vector& vec : plan.vectors)
				if(vec.kind == VectorKind::Load)
					ptrs.push_back({vec.lowestLoad->getPointer(), vec.lowestLoad->getControl()});
			for(StoreNode* st : plan.seed.lowestStores)
				ptrs.push_back({st->getPointer(), st->getControl()});
			Map<U64, Pair<Node*, I64>> best;
			for(const auto& [ptr, ctrl] : ptrs) {
				Pair<Node*, I64> anchor = anchorOf(ptr);
				U64 key = pairKey(ctrl->getId(), addrs.get(ptr).group);
				auto it = best.try_emplace(key, anchor).first;
				if(anchor.second < it->second.second)
					it->second = anchor;
			}
			for(const auto& [key, anchor] : best)
				anchors.try_emplace(key, anchor);
		}

		// the address at offset in ptr's group, as its anchor plus a constant, so lowering folds it
		Node* Slp::anchorPtr(Node* ptr, I64 offset, const Node* ctrl) {
			U64 key = pairKey(ctrl->getId(), addrs.get(ptr).group);
			auto it = anchors.find(key);
			assert(it != anchors.end() && "seedAnchors covers every pointer of the plan");
			auto [anchor, anchorOffset] = it->second;
			if(offset == anchorOffset)
				return anchor;
			Node* delta = fn.create<ConstantNode>(fn.types().getInt(64), offset - anchorOffset);
			return fn.create<BinaryNode>(Opcode::Add, ptr->getType(), anchor, delta);
		}

		// the base of ptr's constant adds when it shares ptr's group, else ptr, with its offset
		Pair<Node*, I64> Slp::anchorOf(Node* ptr) {
			Node* base = ptr;
			BinaryNode* add = dyn_cast<BinaryNode>(base);
			while(add && add->getOpcode() == Opcode::Add && isa<ConstantNode>(add->getRHS()) &&
						add->getLHS()->getType()->isPtr()) {
				base = add->getLHS();
				add = dyn_cast<BinaryNode>(base);
			}
			Addr ptrAddr = addrs.get(ptr);
			Addr baseAddr = addrs.get(base);
			if(baseAddr.group != ptrAddr.group)
				return {ptr, ptrAddr.offset};
			return {base, baseAddr.offset};
		}

		Node* Slp::emitWideStores(const Plan& plan, Node* ctrl) {
			const Seed& seed = plan.seed;
			Node* state = memIn(seed);
			for(U32 window = 0; window < seed.lowestStores.size(); ++window) {
				StoreNode* lowest = seed.lowestStores[window];
				I64 offset = addrs.get(lowest->getPointer()).offset;
				Node* ptr = anchorPtr(lowest->getPointer(), offset, lowest->getControl());
				state = fn.create<StoreNode>(fn.memTy(), ctrl, state, ptr, plan.tupleNode(window));
			}
			return state;
		}

		void Slp::commitRun(const Plan& plan) {
			const Seed& seed = plan.seed;
			Node* wide = emitWideStores(plan, seed.stores[0]->getControl());
			if(MemMergeNode* set = setOf(seed)) {
				U32 firstSlot = ~0u;
				for(StoreNode* st : seed.stores) {
					assert(st->getUsers().size() == 1 && "a set member's only user is its set");
					U32 slot = st->getUserSlot(0);
					firstSlot = std::min(firstSlot, slot);
					set->setInput(slot, nullptr);
				}
				set->setInput(firstSlot, wide);
			} else {
				seed.stores.back()->replaceAllUsesWith(wide);
			}
			for(StoreNode* st : seed.stores)
				st->clearInputs(); // dead without inputs, the sweep in run erases it
		}
	} // namespace slp
} // namespace rat
