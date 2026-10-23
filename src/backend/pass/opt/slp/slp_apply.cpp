#include "pass/opt/slp/slp_pack.h"

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
			if(!vec.wideLoads)
				return fn.create<SplatNode>(vecTy,
																		loadAt(vec, vec.lowestLoad->getType(), vec.offset, plan));
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
			const Addr& ptrAddr = addrs.get(ptr);
			const Addr& baseAddr = addrs.get(base);
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
			if(seed.set) {
				U32 firstSlot = ~0u;
				for(StoreNode* st : seed.stores) {
					assert(st->getUsers().size() == 1 && "a set member's only user is its set");
					U32 slot = st->getUserSlot(0);
					firstSlot = std::min(firstSlot, slot);
					seed.set->setInput(slot, nullptr);
				}
				seed.set->setInput(firstSlot, wide);
			} else {
				seed.stores.back()->replaceAllUsesWith(wide);
			}
			for(StoreNode* st : seed.stores)
				st->clearInputs(); // dead without inputs, the sweep in run erases it
		}

		void Slp::commitGuarded(const Plan& plan) {
			const Seed& seed = plan.seed;
			assert(!seed.set && "no load reads a set member, so no lane edge is weak");
			StoreNode* last = seed.stores.back();
			Node* block = last->getControl();
			Node* firstIf = nullptr;
			Node* elseP = nullptr;
			Node* thenP = emitGuardCascade(plan, firstIf, elseP);
			for(const auto& [ld, addr] : plan.loads)
				if(ld->getControl() == block && ld->getMemory() == memIn(seed))
					ld->setInput(0, thenP);
			Node* wide = emitWideStores(plan, thenP);
			Node* region = fn.create<RegionNode>(fn.ctrlTy(), List<Node*>{thenP, elseP});
			PhiNode* phi = fn.create<PhiNode>(fn.memTy(), List<Node*>{region, wide, last});
			last->replaceAllUsesWith(phi);
			phi->setInput(2, last); // replaceAllUsesWith also rewired the phi's own else input
			moveToElse(plan, block, elseP);
			splits.push_back({block, seed.stores[0], phi, firstIf, region});
			++guardedRuns[block];
		}

		void Slp::moveToElse(const Plan& plan, Node* block, Node* elseP) {
			const Seed& seed = plan.seed;
			for(StoreNode* st : seed.stores)
				st->setInput(0, elseP);
			for(const Vector& vec : plan.vectors)
				if(vec.kind == VectorKind::Load)
					for(Node* lane : vec.lanes) {
						LoadNode* ld = cast<LoadNode>(lane);
						B32 runState = ld->getMemory() == memIn(seed) || isInner(seed, ld->getMemory());
						if(ld->getControl() == block && runState && laneDies(ld))
							ld->setInput(0, elseP);
					}
		}

		// per load group, two branches test that its range ends before the run or starts after it
		Node* Slp::emitGuardCascade(const Plan& plan, Node*& firstIf, Node*& elseP) {
			const Seed& seed = plan.seed;
			Type* ctrlTy = fn.ctrlTy();
			Type* iffTy = fn.types().getTuple({ctrlTy, ctrlTy});
			Node* runPtr = seed.lowestStores[0]->getPointer();
			U32 elemSize = seed.lowestStores[0]->getValue()->getType()->byteSize(ptrBytes);
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

		void Slp::splitBlocks() {
			Map<const Node*, U32> runAt; // memIn -> guarded run
			Set<const Node*> firstIfs;
			for(U32 i = 0; i < splits.size(); ++i) {
				runAt[splits[i].firstStore->getMemory()] = i;
				firstIfs.insert(splits[i].firstIf);
			}
			for(const GuardSplit& split : splits) {
				if(moveLaterUsers(split, runAt))
					continue;
				for(U32 k = split.block->getUsers().size(); k-- > 0;) {
					Node* user = split.block->getUsers()[k];
					if(isControlNode(user) && !firstIfs.count(user))
						user->setInput(split.block->getUserSlot(k), split.region);
				}
			}
		}

		// move the users of the split block that read the merged state or a later one below the merge
		// region, hang a later guarded run of the block below it, and return whether one was found
		B32 Slp::moveLaterUsers(const GuardSplit& split, const Map<const Node*, U32>& runAt) {
			B32 foundLater = false;
			List<Node*> states = {split.phi};
			while(!states.empty()) {
				Node* state = states.back();
				states.pop_back();
				if(auto it = runAt.find(state);
					 it != runAt.end() && splits[it->second].block == split.block) {
					splits[it->second].firstIf->setInput(0, split.region); // input 0 is the branch control
					foundLater = true;
				}
				for(Node* user : state->getUsers()) {
					if(memoryInput(user) != state || user->getInput(0) != split.block)
						continue;
					user->setInput(0, split.region); // input 0 is the control of an anchored access
					if(user->getType()->isMemory())
						states.push_back(user);
					else if(user->getType()->isTuple() && user->projection(CallNode::memoryProjIndex()))
						states.push_back(user->projection(CallNode::memoryProjIndex())); // a call or asm
				}
			}
			return foundLater;
		}

		// combine the vectors by the op, fold the lanes by shuffles, then the other terms as scalars
		void Slp::commitReduction(const Plan& plan) {
			const Seed& seed = plan.seed;
			Opcode op = seed.root->getOpcode();
			Node* acc = plan.tupleNode(0);
			Type* vecTy = acc->getType();
			for(U32 i = 1; i < plan.tupleVectors.size(); ++i)
				acc = fn.create<BinaryNode>(op, vecTy, acc, plan.tupleNode(i));
			U32 laneCount = vecTy->getVecLanes();
			// finish shuffles by [laneCount / 4][round], nibble i is the source of lane i
			constexpr U16 kFinishSel[2][2] = {{0x01}, {0x1032, 0x2301}};
			for(U32 round = 0; round < laneCount / 2; ++round) {
				Node* swapped = fn.create<ShuffleNode>(vecTy, acc, acc, kFinishSel[laneCount / 4][round]);
				acc = fn.create<BinaryNode>(op, vecTy, acc, swapped);
			}
			Node* result = fn.create<ExtractNode>(seed.root->getType(), acc, 0);
			for(Node* term : seed.scalarTerms)
				result = fn.create<BinaryNode>(op, seed.root->getType(), result, term);
			seed.root->replaceAllUsesWith(result);
		}
	} // namespace slp
} // namespace rat
