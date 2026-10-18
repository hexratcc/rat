#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		void Slp::apply(const Seed& seed, const Plan& plan) {
			Made made = emit(plan);
			if(seed.root)
				commitReduction(seed, made);
			else if(plan.guards.empty())
				commitRun(seed, made);
			else
				commitGuarded(seed, plan, made);
		}

		// the node of each record in plan order, operands first, and the vector of each seed tuple
		Made Slp::emit(const Plan& plan) {
			Made made;
			for(const Record& r : plan.records) {
				Type* vecTy = fn.types().getVec(r.lanes[0]->getType(), r.lanes.size());
				if(r.kind == Kind::Load)
					made.nodes.push_back(emitLoad(r, vecTy, made));
				else if(r.kind == Kind::Op)
					made.nodes.push_back(emitOp(r, vecTy, made));
				else
					made.nodes.push_back(emitGather(r, vecTy));
			}
			for(U32 at : plan.tuples)
				made.vectors.push_back(made.nodes[at]);
			return made;
		}

		Node* Slp::emitGather(const Record& r, Type* vecTy) {
			if(oneScalar(r.lanes))
				return fn.create<SplatNode>(vecTy, r.lanes[0]);
			List<Node*> scalars(r.lanes.begin(), r.lanes.end());
			return fn.create<PackNode>(vecTy, scalars);
		}

		// one load at off and a broadcast, or the wide loads, shuffled when the lanes are permuted
		Node* Slp::emitLoad(const Record& r, Type* vecTy, Made& made) {
			if(!r.wideLoads) {
				Node* ptr = anchorPtr(r.lowest->getPointer(), r.off);
				LoadNode* ld = fn.create<LoadNode>(r.lowest->getType(), r.lowest->getControl(), r.mem, ptr);
				made.loads.push_back({ld, addrs.get(r.lowest->getPointer())});
				return fn.create<SplatNode>(vecTy, ld);
			}
			Node* a = wideLoad(r, vecTy, r.off, made);
			if(!r.shuffle)
				return a;
			Node* b = a;
			if(r.wideLoads == 2)
				b = wideLoad(r, vecTy, r.off + (I64)kVecBytes, made);
			return fn.create<ShuffleNode>(vecTy, a, b, r.sel);
		}

		// a vector load of r's state at offset off of its group, or the load an earlier record made
		Node* Slp::wideLoad(const Record& r, Type* vecTy, I64 off, Made& made) {
			Addr a = addrs.get(r.lowest->getPointer());
			a.offset = off;
			for(const auto& [ld, b] : made.loads)
				if(ld->getType() == vecTy && ld->getMemory() == r.mem && b.group == a.group &&
					 b.offset == off && ld->getControl() == r.lowest->getControl())
					return ld;
			Node* ptr = anchorPtr(r.lowest->getPointer(), off);
			LoadNode* ld = fn.create<LoadNode>(vecTy, r.lowest->getControl(), r.mem, ptr);
			made.loads.push_back({ld, a});
			return ld;
		}

		// op over the operand vectors
		Node* Slp::emitOp(const Record& r, Type* vecTy, const Made& made) {
			Node* in[kMaxInputs] = {};
			for(U32 i = 0; i < kMaxInputs; ++i)
				if(r.in[i] != kNone)
					in[i] = made.nodes[r.in[i]];
			return makeOp(r.op, vecTy, in);
		}

		Node* Slp::makeOp(Opcode op, Type* vecTy, Node* const* in) {
			return fn.create<BinaryNode>(op, vecTy, in[0], in[1]);
		}

		// offset off of ptr's group as the group's anchor plus a constant, so lowering folds it
		Node* Slp::anchorPtr(Node* ptr, I64 off) {
			U32 group = addrs.get(ptr).group;
			auto it = anchors.find(group);
			if(it == anchors.end())
				it = anchors.emplace(group, anchorOf(ptr)).first;
			auto [anchor, anchorOff] = it->second;
			if(off == anchorOff)
				return anchor;
			Node* delta = fn.create<ConstantNode>(fn.types().getInt(64), off - anchorOff);
			return fn.create<BinaryNode>(Opcode::Add, ptr->getType(), anchor, delta);
		}

		Pair<Node*, I64> Slp::anchorOf(Node* ptr) {
			Node* base = ptr;
			BinaryNode* b = dyn_cast<BinaryNode>(base);
			while(b && b->getOpcode() == Opcode::Add && isa<ConstantNode>(b->getRHS()) &&
						b->getLHS()->getType()->isPtr()) {
				base = b->getLHS();
				b = dyn_cast<BinaryNode>(base);
			}
			const Addr &a = addrs.get(ptr), &baseA = addrs.get(base);
			if(baseA.group != a.group)
				return {ptr, a.offset};
			return {base, baseA.offset};
		}

		Node* Slp::chainWideStores(const Seed& seed, const Made& made, Node* ctrl) {
			Node* mem = seed.memIn;
			for(U32 k = 0; k < seed.heads.size(); ++k) {
				Node* ptr = seed.heads[k]->getPointer();
				ptr = anchorPtr(ptr, addrs.get(ptr).offset);
				mem = fn.create<StoreNode>(fn.memTy(), ctrl, mem, ptr, made.vectors[k]);
			}
			return mem;
		}

		void Slp::commitRun(const Seed& seed, const Made& made) {
			StoreNode* last = seed.stores.back();
			replaceUses(last, chainWideStores(seed, made, last->getControl()));
			for(StoreNode* st : seed.stores)
				st->clearInputs(); // dead without inputs, the sweep in run erases it
			stats.packedUnguarded += seed.windows;
		}

		void Slp::commitGuarded(const Seed& seed, const Plan& plan, const Made& made) {
			StoreNode* last = seed.stores.back();
			Node* block = last->getControl();
			Node *iff = nullptr, *elseP = nullptr;
			Node* thenP = emitGuardCascade(seed, plan, iff, elseP);
			for(const auto& [ld, addr] : made.loads)
				if(ld->getControl() == block && ld->getMemory() == seed.memIn)
					ld->setInput(0, thenP);
			Node* wide = chainWideStores(seed, made, thenP);
			Node* region = fn.create<RegionNode>(fn.ctrlTy(), List<Node*>{thenP, elseP});
			PhiNode* phi = fn.create<PhiNode>(fn.memTy(), List<Node*>{region, wide, last});
			replaceUses(last, phi);
			phi->setInput(2, last); // replaceUses also rewired the phi's own else input
			moveToElse(seed, plan, block, elseP);
			splits.push_back({block, seed.memIn, phi, iff, region});
			guardedBlocks.insert(block);
			stats.packedGuarded += seed.windows;
			stats.guardedRuns += 1;
			stats.guardPairs += (U32)plan.guards.size();
		}

		void Slp::moveToElse(const Seed& seed, const Plan& plan, Node* block, Node* elseP) {
			for(StoreNode* st : seed.stores)
				st->setInput(0, elseP);
			for(const Record& r : plan.records)
				for(U32 i = 0; r.kind == Kind::Load && i < r.lanes.size(); ++i) {
					LoadNode* ld = cast<LoadNode>(r.lanes[i]);
					B32 runState = ld->getMemory() == seed.memIn || isInner(seed, ld->getMemory());
					if(ld->getControl() == block && runState && dies(ld))
						ld->setInput(0, elseP);
				}
		}

		// per load group, two branches test that its range ends before the run or starts after it
		Node* Slp::emitGuardCascade(const Seed& seed, const Plan& plan, Node*& iff, Node*& elseP) {
			Type* ctrlTy = fn.ctrlTy();
			Type* iffTy = fn.types().getTuple({ctrlTy, ctrlTy});
			Node* wPtr = seed.heads[0]->getPointer();
			U32 esz = seed.heads[0]->getValue()->getType()->byteSize(ptrBytes);
			I64 bytes = (I64)(seed.stores.size() * esz);
			Node* runBytes = fn.create<ConstantNode>(fn.types().getInt(64), bytes);
			Node* wEnd = fn.create<BinaryNode>(Opcode::Add, wPtr->getType(), wPtr, runBytes);
			Node* ctrl = seed.stores.back()->getControl();
			List<Node*> fails;
			for(const GuardGroup& g : plan.guards) {
				Node* gBytes = fn.create<ConstantNode>(fn.types().getInt(64), g.hi - g.lo);
				Node* gEnd = fn.create<BinaryNode>(Opcode::Add, g.ptr->getType(), g.ptr, gBytes);
				Node* before = fn.create<CompareNode>(Opcode::Ule, fn.boolTy(), gEnd, wPtr);
				Node* bIf = fn.create<IfNode>(iffTy, ctrl, before);
				Node* bT = fn.create<ProjNode>(ctrlTy, bIf, IfNode::thenProjIndex(), "slp.then");
				Node* bF = fn.create<ProjNode>(ctrlTy, bIf, IfNode::elseProjIndex(), "slp.chk");
				Node* after = fn.create<CompareNode>(Opcode::Ule, fn.boolTy(), wEnd, g.ptr);
				Node* aIf = fn.create<IfNode>(iffTy, bF, after);
				Node* aT = fn.create<ProjNode>(ctrlTy, aIf, IfNode::thenProjIndex(), "slp.then");
				if(fails.empty())
					iff = bIf;
				fails.push_back(fn.create<ProjNode>(ctrlTy, aIf, IfNode::elseProjIndex(), "slp.else"));
				ctrl = fn.create<RegionNode>(ctrlTy, List<Node*>{bT, aT});
			}
			elseP = fails[0];
			if(fails.size() > 1)
				elseP = fn.create<RegionNode>(ctrlTy, fails);
			return ctrl;
		}

		void Slp::splitBlocks() {
			Map<const Node*, U32> runAt; // memIn -> guarded run
			Set<const Node*> iffs;
			for(U32 r = 0; r < splits.size(); ++r) {
				runAt[splits[r].memIn] = r;
				iffs.insert(splits[r].iff);
			}
			for(const GuardSplit& g : splits) {
				if(moveLaterUsers(g, runAt))
					continue;
				for(U32 k = g.block->getUsers().size(); k-- > 0;) {
					Node* u = g.block->getUsers()[k];
					if(isControlNode(u) && !iffs.count(u))
						u->setInput(g.block->getUserSlot(k), g.region);
				}
			}
		}

		// move the users of g's block that read the merged state or a later one below g's merge region
		B32 Slp::moveLaterUsers(const GuardSplit& g, const Map<const Node*, U32>& runAt) {
			B32 next = false;
			List<Node*> states = {g.phi}, moved;
			while(!states.empty()) {
				Node* m = states.back();
				states.pop_back();
				if(auto it = runAt.find(m); it != runAt.end() && splits[it->second].block == g.block) {
					splits[it->second].iff->setInput(0, g.region); // input 0 is the branch control
					next = true;
				}
				moved.clear();
				for(Node* u : m->getUsers())
					if(memoryOperand(u) == m && u->getInput(0) == g.block)
						moved.push_back(u);
				for(Node* u : moved) {
					u->setInput(0, g.region); // input 0 is the control of an anchored access
					if(u->getType()->isMemory())
						states.push_back(u);
					else if(u->getType()->isTuple() && u->projection(CallNode::memoryProjIndex()))
						states.push_back(u->projection(CallNode::memoryProjIndex())); // a call or asm
				}
			}
			return next;
		}

		// combine the vectors by the op, fold the lanes by shuffles, then the other terms as scalars
		void Slp::commitReduction(const Seed& seed, const Made& made) {
			const List<Node*>& vecs = made.vectors;
			Opcode op = seed.root->getOpcode();
			Type* vecTy = vecs[0]->getType();
			Node* acc = vecs[0];
			for(U32 v = 1; v < vecs.size(); ++v)
				acc = fn.create<BinaryNode>(op, vecTy, acc, vecs[v]);
			U32 w = vecTy->getVecLanes();
			for(U32 r = 0; r < w / 2; ++r) {
				Node* half = fn.create<ShuffleNode>(vecTy, acc, acc, kFinishSel[w / 4][r]);
				acc = fn.create<BinaryNode>(op, vecTy, acc, half);
			}
			Node* res = fn.create<ExtractNode>(seed.root->getType(), acc, 0);
			for(Node* r : seed.rest)
				res = fn.create<BinaryNode>(op, seed.root->getType(), res, r);
			replaceUses(seed.root, res);
			++stats.packedReduction;
		}
	} // namespace slp
} // namespace rat
