#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		void Slp::apply(const Seed& seed, const Plan& plan) {
			seedAnchors(seed, plan);
			Emitted emitted = emitPlan(plan);
			if(seed.root)
				commitReduction(seed, emitted);
			else if(plan.guards.empty())
				commitRun(seed, emitted);
			else
				commitGuarded(seed, plan, emitted);
		}

		Emitted Slp::emitPlan(const Plan& plan) {
			Emitted emitted;
			for(const Vector& vec : plan.vectors) {
				Type* vecTy = fn.types().getVec(laneType(vec.lanes[0]), vec.lanes.size());
				if(vec.kind == VectorKind::Load)
					emitted.nodes.push_back(emitLoad(vec, vecTy, emitted));
				else if(vec.kind == VectorKind::Op)
					emitted.nodes.push_back(emitOp(vec, vecTy, emitted));
				else
					emitted.nodes.push_back(emitGather(vec, vecTy));
			}
			for(U32 index : plan.tupleVectors)
				emitted.tupleNodes.push_back(emitted.nodes[index]);
			return emitted;
		}

		Node* Slp::emitGather(const Vector& vec, Type* vecTy) {
			if(isSplat(vec.lanes))
				return fn.create<SplatNode>(vecTy, vec.lanes[0]);
			List<Node*> scalars(vec.lanes.begin(), vec.lanes.end());
			return fn.create<PackNode>(vecTy, scalars);
		}

		Node* Slp::emitLoad(const Vector& vec, Type* vecTy, Emitted& emitted) {
			if(!vec.wideLoads)
				return fn.create<SplatNode>(vecTy,
																		loadAt(vec, vec.lowestLoad->getType(), vec.offset, emitted));
			Node* low = loadAt(vec, vecTy, vec.offset, emitted);
			if(!vec.permuted)
				return low;
			Node* high = low;
			if(vec.wideLoads == 2)
				high = loadAt(vec, vecTy, vec.offset + (I64)kVecBytes, emitted);
			return fn.create<ShuffleNode>(vecTy, low, high, vec.sel);
		}

		// a load of type ty at offset of vec's group from vec.mem, or an earlier vector's equal load
		Node* Slp::loadAt(const Vector& vec, Type* ty, I64 offset, Emitted& emitted) {
			Addr addr = addrs.get(vec.lowestLoad->getPointer());
			addr.offset = offset;
			for(const auto& [ld, ldAddr] : emitted.loads)
				if(ld->getType() == ty && ld->getMemory() == vec.mem && ldAddr.group == addr.group &&
					 ldAddr.offset == offset && ld->getControl() == vec.lowestLoad->getControl())
					return ld;
			Node* ptr = anchorPtr(vec.lowestLoad->getPointer(), offset);
			LoadNode* ld = fn.create<LoadNode>(ty, vec.lowestLoad->getControl(), vec.mem, ptr);
			emitted.loads.push_back({ld, addr});
			return ld;
		}

		// mainOp over the operand vectors, or mainOp and altOp blended by sel
		Node* Slp::emitOp(const Vector& vec, Type* vecTy, const Emitted& emitted) {
			Node* inputs[kMaxInputs] = {};
			for(U32 i = 0; i < kMaxInputs; ++i)
				if(vec.operands[i] != kNoVector)
					inputs[i] = emitted.nodes[vec.operands[i]];
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

		// a group the plan is the first to use takes the anchorOf base of the lowest offset among the
		// plan's pointers of the group, the first in plan order on a tie
		void Slp::seedAnchors(const Seed& seed, const Plan& plan) {
			List<Node*> ptrs;
			for(const Vector& vec : plan.vectors)
				if(vec.kind == VectorKind::Load)
					ptrs.push_back(vec.lowestLoad->getPointer());
			for(StoreNode* st : seed.lowestStores)
				ptrs.push_back(st->getPointer());
			Map<U32, Pair<Node*, I64>> best;
			for(Node* ptr : ptrs) {
				Pair<Node*, I64> anchor = anchorOf(ptr);
				auto it = best.try_emplace(addrs.get(ptr).group, anchor).first;
				if(anchor.second < it->second.second)
					it->second = anchor;
			}
			for(const auto& [group, anchor] : best)
				anchors.try_emplace(group, anchor);
		}

		// the address at offset in ptr's group, as its anchor plus a constant, so lowering folds it
		Node* Slp::anchorPtr(Node* ptr, I64 offset) {
			auto it = anchors.find(addrs.get(ptr).group);
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

		Node* Slp::emitWideStores(const Seed& seed, const Emitted& emitted, Node* ctrl) {
			Node* state = seed.memIn;
			for(U32 window = 0; window < seed.lowestStores.size(); ++window) {
				Node* ptr = seed.lowestStores[window]->getPointer();
				ptr = anchorPtr(ptr, addrs.get(ptr).offset);
				state = fn.create<StoreNode>(fn.memTy(), ctrl, state, ptr, emitted.tupleNodes[window]);
			}
			return state;
		}

		void Slp::commitRun(const Seed& seed, const Emitted& emitted) {
			Node* wide = emitWideStores(seed, emitted, seed.stores[0]->getControl());
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
			stats.packedUnguarded += seed.windows;
		}

		void Slp::commitGuarded(const Seed& seed, const Plan& plan, const Emitted& emitted) {
			assert(!seed.set && "no load reads a set member, so no lane edge is weak");
			StoreNode* last = seed.stores.back();
			Node* block = last->getControl();
			Node* firstIf = nullptr;
			Node* elseP = nullptr;
			Node* thenP = emitGuardCascade(seed, plan, firstIf, elseP);
			for(const auto& [ld, addr] : emitted.loads)
				if(ld->getControl() == block && ld->getMemory() == seed.memIn)
					ld->setInput(0, thenP);
			Node* wide = emitWideStores(seed, emitted, thenP);
			Node* region = fn.create<RegionNode>(fn.ctrlTy(), List<Node*>{thenP, elseP});
			PhiNode* phi = fn.create<PhiNode>(fn.memTy(), List<Node*>{region, wide, last});
			last->replaceAllUsesWith(phi);
			phi->setInput(2, last); // replaceAllUsesWith also rewired the phi's own else input
			moveToElse(seed, plan, block, elseP);
			splits.push_back({block, seed.stores[0], phi, firstIf, region});
			++guardedRuns[block];
			stats.packedGuarded += seed.windows;
			stats.guardedRuns += 1;
			stats.guardChecks += (U32)plan.guards.size();
		}

		void Slp::moveToElse(const Seed& seed, const Plan& plan, Node* block, Node* elseP) {
			for(StoreNode* st : seed.stores)
				st->setInput(0, elseP);
			for(const Vector& vec : plan.vectors)
				if(vec.kind == VectorKind::Load)
					for(Node* lane : vec.lanes) {
						LoadNode* ld = cast<LoadNode>(lane);
						B32 runState = ld->getMemory() == seed.memIn || isInner(seed, ld->getMemory());
						if(ld->getControl() == block && runState && laneDies(ld))
							ld->setInput(0, elseP);
					}
		}

		// per load group, two branches test that its range ends before the run or starts after it
		Node* Slp::emitGuardCascade(const Seed& seed, const Plan& plan, Node*& firstIf, Node*& elseP) {
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
		void Slp::commitReduction(const Seed& seed, const Emitted& emitted) {
			const List<Node*>& vectors = emitted.tupleNodes;
			Opcode op = seed.root->getOpcode();
			Type* vecTy = vectors[0]->getType();
			Node* acc = vectors[0];
			for(U32 i = 1; i < vectors.size(); ++i)
				acc = fn.create<BinaryNode>(op, vecTy, acc, vectors[i]);
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
			++stats.packedReduction;
		}
	} // namespace slp
} // namespace rat
