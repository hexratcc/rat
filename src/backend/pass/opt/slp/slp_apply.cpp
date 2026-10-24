#include "pass/opt/slp/slp_pack_detail.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		void Slp::apply(Plan& plan) {
			const Seed& seed = plan.seed;
			emitter.seedAnchors(plan);
			emitter.emitVectors(plan);
			if(seed.root) {
				emitter.commitReduction(plan);
				++stats.packedReduction;
			} else if(plan.guards.empty()) {
				emitter.commitRun(plan);
				stats.packedUnguarded += (U32)seed.lowestStores.size();
			} else {
				emitter.commitGuarded(plan, check);
				stats.packedGuarded += (U32)seed.lowestStores.size();
				++stats.guardedRuns;
				stats.guardChecks += (U32)plan.guards.size();
			}
		}

		Emitter::Emitter(Context& cx)
		: cx(cx) {}

		// operands first, so an Op finds the nodes of its operands
		void Emitter::emitVectors(Plan& plan) {
			for(Vector& vec : plan.vectors) {
				Type* vecTy = cx.fn.types().getVec(cx.laneType(vec.lanes[0]), vec.lanes.size());
				if(vec.kind == VectorKind::Load)
					vec.node = emitLoad(vec, vecTy, plan);
				else if(vec.kind == VectorKind::Op)
					vec.node = emitOp(vec, vecTy, plan);
				else
					vec.node = emitGather(vec, vecTy);
			}
		}

		Node* Emitter::emitGather(const Vector& vec, Type* vecTy) {
			if(isSplat(vec.lanes))
				return cx.fn.create<SplatNode>(vecTy, vec.lanes[0]);
			List<Node*> scalars(vec.lanes.begin(), vec.lanes.end());
			return cx.fn.create<PackNode>(vecTy, scalars);
		}

		Node* Emitter::emitLoad(const Vector& vec, Type* vecTy, Plan& plan) {
			if(!vec.wideLoads) {
				Node* load = loadAt(vec, vec.lowestLoad->getType(), vec.offset, plan);
				return cx.fn.create<SplatNode>(vecTy, load);
			}
			Node* low = loadAt(vec, vecTy, vec.offset, plan);
			if(!vec.permuted)
				return low;
			Node* high = low;
			if(vec.wideLoads == 2)
				high = loadAt(vec, vecTy, vec.offset + (I64)kVecBytes, plan);
			return cx.fn.create<ShuffleNode>(vecTy, low, high, vec.sel);
		}

		// a load of type ty at offset of vec's group from vec.mem, or an earlier vector's equal load
		Node* Emitter::loadAt(const Vector& vec, Type* ty, I64 offset, Plan& plan) {
			Addr addr = cx.addrs.get(vec.lowestLoad->getPointer());
			addr.offset = offset;
			for(const auto& [ld, ldAddr] : plan.loads)
				if(ld->getType() == ty && ld->getMemory() == vec.mem && ldAddr.group == addr.group &&
					 ldAddr.offset == offset && ld->getControl() == vec.lowestLoad->getControl())
					return ld;
			Node* ptr = anchorPtr(vec.lowestLoad->getPointer(), offset, vec.lowestLoad->getControl());
			LoadNode* ld = cx.fn.create<LoadNode>(ty, vec.lowestLoad->getControl(), vec.mem, ptr);
			plan.loads.push_back({ld, addr});
			return ld;
		}

		// mainOp over the operand vectors, or mainOp and altOp blended by sel
		Node* Emitter::emitOp(const Vector& vec, Type* vecTy, const Plan& plan) {
			Node* inputs[kMaxInputs] = {};
			for(U32 i = 0; i < kMaxInputs; ++i)
				if(vec.operands[i] != kNoVector)
					inputs[i] = plan.vectors[vec.operands[i]].node;
			if(vec.shiftAmount)
				inputs[1] = vec.shiftAmount;
			Node* result = makeOp(vec.mainOp, vecTy, inputs);
			if(vec.altOp == kNoOp)
				return result;
			return cx.fn.create<ShuffleNode>(vecTy, result, makeOp(vec.altOp, vecTy, inputs), vec.sel);
		}

		Node* Emitter::makeOp(Opcode op, Type* vecTy, Node* const* inputs) {
			switch(getOpClass(op)) {
			case OpClass::Binary:
				return cx.fn.create<BinaryNode>(op, vecTy, inputs[0], inputs[1]);
			case OpClass::Unary:
				return cx.fn.create<UnaryNode>(op, vecTy, inputs[0]);
			case OpClass::Compare:
				return cx.fn.create<CompareNode>(op, vecTy, inputs[0], inputs[1]);
			case OpClass::Convert:
				return cx.fn.create<ConvertNode>(op, vecTy, inputs[0]);
			default:
				return cx.fn.create<SelectNode>(vecTy, inputs[0], inputs[1], inputs[2]);
			}
		}

		// a (control, group) the plan is the first to use takes the anchorOf base of the lowest offset
		// among the plan's pointers of it, the first in plan order on a tie. A group's equal addresses
		// can rest on nsw facts that hold only where its accesses run, so an anchor stays in its control
		void Emitter::seedAnchors(const Plan& plan) {
			List<Pair<Node*, const Node*>> ptrs;
			for(const Vector& vec : plan.vectors)
				if(vec.kind == VectorKind::Load)
					ptrs.push_back({vec.lowestLoad->getPointer(), vec.lowestLoad->getControl()});
			for(StoreNode* st : plan.seed.lowestStores)
				ptrs.push_back({st->getPointer(), st->getControl()});
			Map<U64, Pair<Node*, I64>> best;
			for(const auto& [ptr, ctrl] : ptrs) {
				Pair<Node*, I64> anchor = anchorOf(ptr);
				U64 key = pairKey(ctrl->getId(), cx.addrs.get(ptr).group);
				auto it = best.try_emplace(key, anchor).first;
				if(anchor.second < it->second.second)
					it->second = anchor;
			}
			for(const auto& [key, anchor] : best)
				anchors.try_emplace(key, anchor);
		}

		// the address at offset in ptr's group, as its anchor plus a constant, so lowering folds it
		Node* Emitter::anchorPtr(Node* ptr, I64 offset, const Node* ctrl) {
			U64 key = pairKey(ctrl->getId(), cx.addrs.get(ptr).group);
			auto it = anchors.find(key);
			assert(it != anchors.end() && "seedAnchors covers every pointer of the plan");
			auto [anchor, anchorOffset] = it->second;
			if(offset == anchorOffset)
				return anchor;
			Node* delta = cx.fn.create<ConstantNode>(cx.fn.types().getInt(64), offset - anchorOffset);
			return cx.fn.create<BinaryNode>(Opcode::Add, ptr->getType(), anchor, delta);
		}

		// the base of ptr's constant adds when it shares ptr's group, else ptr, with its offset
		Pair<Node*, I64> Emitter::anchorOf(Node* ptr) {
			Node* base = ptr;
			BinaryNode* add = dyn_cast<BinaryNode>(base);
			while(add && add->getOpcode() == Opcode::Add && isa<ConstantNode>(add->getRHS()) &&
						add->getLHS()->getType()->isPtr()) {
				base = add->getLHS();
				add = dyn_cast<BinaryNode>(base);
			}
			Addr ptrAddr = cx.addrs.get(ptr);
			Addr baseAddr = cx.addrs.get(base);
			if(baseAddr.group != ptrAddr.group)
				return {ptr, ptrAddr.offset};
			return {base, baseAddr.offset};
		}

		Node* Emitter::emitWideStores(const Plan& plan, Node* ctrl) {
			const Seed& seed = plan.seed;
			Node* state = memIn(seed);
			for(U32 window = 0; window < seed.lowestStores.size(); ++window) {
				StoreNode* lowest = seed.lowestStores[window];
				I64 offset = cx.addrs.get(lowest->getPointer()).offset;
				Node* ptr = anchorPtr(lowest->getPointer(), offset, lowest->getControl());
				state = cx.fn.create<StoreNode>(cx.fn.memTy(), ctrl, state, ptr, plan.tupleNode(window));
			}
			return state;
		}

		void Emitter::commitRun(const Plan& plan) {
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

		void Emitter::commitGuarded(const Plan& plan, const Check& check) {
			const Seed& seed = plan.seed;
			assert(!setOf(seed) && "no load reads a set member, so no lane edge is weak");
			StoreNode* last = seed.stores.back();
			Node* block = last->getControl();
			Node* firstIf = nullptr;
			Node* elseP = nullptr;
			Node* thenP = emitGuardCascade(plan, firstIf, elseP);
			for(const auto& [ld, addr] : plan.loads)
				if(ld->getControl() == block && ld->getMemory() == memIn(seed))
					ld->setInput(0, thenP);
			Node* wide = emitWideStores(plan, thenP);
			Node* region = cx.fn.create<RegionNode>(cx.fn.ctrlTy(), List<Node*>{thenP, elseP});
			PhiNode* phi = cx.fn.create<PhiNode>(cx.fn.memTy(), List<Node*>{region, wide, last});
			last->replaceAllUsesWith(phi);
			phi->setInput(2, last); // replaceAllUsesWith also rewired the phi's own else input
			moveToElse(plan, block, elseP, check);
			guardedRuns.push_back({block, seed.stores[0], phi, firstIf});
			++cx.guardedRunsPerBlock[block];
		}

		void Emitter::moveToElse(const Plan& plan, Node* block, Node* elseP, const Check& check) {
			const Seed& seed = plan.seed;
			for(StoreNode* st : seed.stores)
				st->setInput(0, elseP);
			for(const Vector& vec : plan.vectors)
				if(vec.kind == VectorKind::Load)
					for(Node* lane : vec.lanes) {
						LoadNode* ld = cast<LoadNode>(lane);
						B32 runState = ld->getMemory() == memIn(seed) || isInner(seed, ld->getMemory());
						if(ld->getControl() == block && runState && check.laneDies(ld))
							ld->setInput(0, elseP);
					}
		}

		// per load group, two branches test that its range ends before the run or starts after it
		Node* Emitter::emitGuardCascade(const Plan& plan, Node*& firstIf, Node*& elseP) {
			const Seed& seed = plan.seed;
			Function& fn = cx.fn;
			Type* ctrlTy = fn.ctrlTy();
			Type* iffTy = fn.types().getTuple({ctrlTy, ctrlTy});
			Node* runPtr = seed.lowestStores[0]->getPointer();
			U32 elemSize = seed.lowestStores[0]->getValue()->getType()->byteSize(cx.ptrBytes);
			I64 runBytes = (I64)(seed.stores.size() * elemSize);
			Node* runSize = fn.create<ConstantNode>(fn.types().getInt(64), runBytes);
			Node* runEnd = fn.create<BinaryNode>(Opcode::Add, runPtr->getType(), runPtr, runSize);
			Node* ctrl = seed.stores.back()->getControl();
			List<Node*> fails;
			for(const GuardRange& range : plan.guards) {
				Node* rangeSize = fn.create<ConstantNode>(fn.types().getInt(64), range.end - range.begin);
				Node* rangeEnd =
						fn.create<BinaryNode>(Opcode::Add, range.ptr->getType(), range.ptr, rangeSize);
				Node* endsBefore = fn.create<CompareNode>(Opcode::Ule, fn.boolTy(), rangeEnd, runPtr);
				Node* before = fn.create<IfNode>(iffTy, ctrl, endsBefore);
				Node* beforeThen = fn.create<ProjNode>(ctrlTy, before, IfNode::thenProjIndex(), "slp.then");
				Node* beforeElse = fn.create<ProjNode>(ctrlTy, before, IfNode::elseProjIndex(), "slp.chk");
				Node* startsAfter = fn.create<CompareNode>(Opcode::Ule, fn.boolTy(), runEnd, range.ptr);
				Node* after = fn.create<IfNode>(iffTy, beforeElse, startsAfter);
				Node* afterThen = fn.create<ProjNode>(ctrlTy, after, IfNode::thenProjIndex(), "slp.then");
				if(fails.empty())
					firstIf = before;
				fails.push_back(fn.create<ProjNode>(ctrlTy, after, IfNode::elseProjIndex(), "slp.else"));
				ctrl = fn.create<RegionNode>(ctrlTy, List<Node*>{beforeThen, afterThen});
			}
			elseP = fails[0];
			if(fails.size() > 1)
				elseP = fn.create<RegionNode>(ctrlTy, fails);
			return ctrl;
		}

		void Emitter::splitBlocks() {
			Map<const Node*, U32> runAt; // memIn -> guarded run
			Set<const Node*> firstIfs;
			for(U32 i = 0; i < guardedRuns.size(); ++i) {
				runAt[guardedRuns[i].firstStore->getMemory()] = i;
				firstIfs.insert(guardedRuns[i].firstIf);
			}
			for(const GuardedRun& guarded : guardedRuns) {
				if(moveLaterUsers(guarded, runAt))
					continue;
				for(U32 k = guarded.block->getUsers().size(); k-- > 0;) {
					Node* user = guarded.block->getUsers()[k];
					if(isControlNode(user) && !firstIfs.count(user))
						user->setInput(guarded.block->getUserSlot(k), guarded.phi->getRegion());
				}
			}
		}

		// move the users of the split block that read the merged state or a later one below the merge
		// region, hang a later guarded run of the block below it, and return whether one was found
		B32 Emitter::moveLaterUsers(const GuardedRun& guarded, const Map<const Node*, U32>& runAt) {
			Node* region = guarded.phi->getRegion();
			B32 foundLater = false;
			List<Node*> states = {guarded.phi};
			while(!states.empty()) {
				Node* state = states.back();
				states.pop_back();
				if(auto it = runAt.find(state);
					 it != runAt.end() && guardedRuns[it->second].block == guarded.block) {
					// input 0 is the branch control
					guardedRuns[it->second].firstIf->setInput(0, region);
					foundLater = true;
				}
				for(Node* user : state->getUsers()) {
					if(memoryInput(user) != state || user->getInput(0) != guarded.block)
						continue;
					user->setInput(0, region); // input 0 is the control of an anchored access
					if(user->getType()->isMemory())
						states.push_back(user);
					else if(user->getType()->isTuple() && user->projection(CallNode::memoryProjIndex()))
						states.push_back(user->projection(CallNode::memoryProjIndex())); // a call or asm
				}
			}
			return foundLater;
		}
	} // namespace slp
} // namespace rat
