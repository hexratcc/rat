#include "pass/opt/slp/slp_pack_detail.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		// add the bytes of vec's lanes to the runtime check of their group
		void Plan::addGuard(const Vector& vec) {
			Addr lowest = cx.addrs.get(vec.lowestLoad->getPointer());
			for(GuardRange& range : guards)
				if(range.group == lowest.group) {
					if(lowest.offset < range.begin)
						range = {range.group, vec.lowestLoad->getPointer(), lowest.offset, range.end};
					range.end = std::max(range.end, vec.end);
					return;
				}
			guards.push_back({lowest.group, vec.lowestLoad->getPointer(), lowest.offset, vec.end});
		}

		GuardedRuns::GuardedRuns(Context& cx)
		: cx(cx) {}

		B32 GuardedRuns::hasRuns(const Node* block) const { return perBlock.count(block) != 0; }

		B32 GuardedRuns::isFull(const Node* block) const {
			auto it = perBlock.find(block);
			return it != perBlock.end() && it->second >= kMaxGuardedRuns;
		}

		void GuardedRuns::commit(const Plan& plan, Emitter& emitter, const Check& check) {
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
			Node* wide = emitter.emitWideStores(plan, thenP);
			Node* region = cx.fn.create<RegionNode>(cx.fn.ctrlTy(), List<Node*>{thenP, elseP});
			PhiNode* phi = cx.fn.create<PhiNode>(cx.fn.memTy(), List<Node*>{region, wide, last});
			last->replaceAllUsesWith(phi);
			phi->setInput(2, last); // replaceAllUsesWith also rewired the phi's own else input
			moveToElse(plan, block, elseP, check);
			runs.push_back({block, seed.stores[0], phi, firstIf});
			++perBlock[block];
		}

		void GuardedRuns::moveToElse(const Plan& plan, Node* block, Node* elseP, const Check& check) {
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
		Node* GuardedRuns::emitGuardCascade(const Plan& plan, Node*& firstIf, Node*& elseP) {
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

		void GuardedRuns::splitBlocks() {
			Map<const Node*, U32> runAt; // memIn -> guarded run
			Set<const Node*> firstIfs;
			for(U32 i = 0; i < runs.size(); ++i) {
				runAt[runs[i].firstStore->getMemory()] = i;
				firstIfs.insert(runs[i].firstIf);
			}
			for(const GuardedRun& guarded : runs) {
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
		B32 GuardedRuns::moveLaterUsers(const GuardedRun& guarded, const Map<const Node*, U32>& runAt) {
			Node* region = guarded.phi->getRegion();
			B32 foundLater = false;
			List<Node*> states = {guarded.phi};
			while(!states.empty()) {
				Node* state = states.back();
				states.pop_back();
				if(auto it = runAt.find(state);
					 it != runAt.end() && runs[it->second].block == guarded.block) {
					// input 0 is the branch control
					runs[it->second].firstIf->setInput(0, region);
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
