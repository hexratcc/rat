// guarded speculation & commit

#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	using namespace slp;

	// true when every operand of v is a compile-time constant
	static B32 allInputsConst(Node* v) {
		for(U32 j = 0, e = v->getInputCount(); j < e; ++j)
			if(!isa<ConstantNode>(v->getInput(j)))
				return false;
		return true;
	}

	U32 SlpPackPass::Slp::tryStaticWindow(Segment& seg, U32 i, const WindowShape& w0) {
		const RefinedAddr& wkey = w0.byOff[0]->key;
		Packer packer(*this, seg[i].store->getMemory(), &wkey);
		packer.collectRun(seg, i, w0.w);
		packer.profit += (I32)w0.w - 1; // the fused store itself
		Node* vec = packer.packTuple(laneValues(w0), w0.elemTy, 0);

		// a splat or an all-constant pack needs no scalar setup, so it is worth fusing
		// even with no interior vector arithmetic
		B32 splat = vec && vec->getOpcode() == Opcode::Splat;
		B32 constPack = vec && vec->getOpcode() == Opcode::Pack && allInputsConst(vec);
		B32 cheapFill = splat || constPack;

		B32 profitable = packer.profit >= kMinProfit && (packer.interior > 0 || cheapFill);
		if(!vec || !packer.guardGroups.empty() || !profitable) {
			if(!vec)
				++stats.rejectedTree;
			else
				++stats.rejectedProfit;
			return 0;
		}

		StoreNode* lastInChain = seg[i + w0.w - 1].store;
		Node* wide = fn.create<StoreNode>(fn.memTy(),
																			w0.ctrl,
																			packer.memIn,
																			packer.anchorPtr(w0.byOff[0]->store->getPointer(), wkey),
																			vec);
		lastInChain->replaceAllUsesWith(wide);
		for(U32 j = 0; j < w0.w; ++j)
			fn.removeNode(seg[i + j].store);
		packer.coalesceSplats();
		++stats.packedUnguarded;
		return w0.w;
	}

	U32 SlpPackPass::Slp::tryGuardedRun(Segment& seg, U32 i, const WindowShape& w0) {
		List<WindowShape> run = {w0};
		while(run.size() < kMaxRunWindows) {
			WindowShape wn;
			if(!windowAt(seg, i + (U32)run.size() * w0.w, wn))
				break;
			B32 sameGroup = wn.byOff[0]->key.sameGroup(w0.byOff[0]->key);
			B32 contiguous = wn.lo == w0.lo + (I64)(run.size() * kVecBytes);
			if(!sameGroup || !contiguous || wn.ctrl != w0.ctrl)
				break;
			run.push_back(std::move(wn));
		}
		U32 n = (U32)run.size();
		U32 total = n * w0.w;
		stats.windowsSeen += n - 1; // run extensions
		Node* wPtr = w0.byOff[0]->store->getPointer();

		Packer packer(*this, seg[i].store->getMemory(), &w0.byOff[0]->key);
		packer.collectRun(seg, i, total);
		List<Node*> vecs;
		B32 treeOk = true;
		for(U32 k = 0; k < n && treeOk; ++k) {
			packer.profit += (I32)w0.w - 1; // each fused store
			if(Node* v = packer.packTuple(laneValues(run[k]), w0.elemTy, 0))
				vecs.push_back(v);
			else
				treeOk = false;
		}

		I32 guardCost = 0;
		if(!guardCostDisabled())
			guardCost = (I32)packer.guardGroups.size() * kGuardCheckCost + kGuardBranchCost;
		B32 profitable = packer.profit - guardCost >= (I32)(kMinProfit * n) && packer.interior >= n;
		B32 withinBudget = packer.guardGroups.size() <= kMaxGuards;
		B32 accept = treeOk && profitable && withinBudget && !packer.coneTouchesObserver(wPtr);
		for(const Packer::GuardGroup& g : packer.guardGroups)
			accept = accept && !packer.coneTouchesObserver(g.ptr);
		// every observer must feed only the scalar stores of this run
		for(const Node* obs : packer.observers)
			accept = accept && coneEndsInStores(obs, packer.runStores, 128);
		if(!accept) {
			if(!treeOk)
				++stats.rejectedTree;
			else
				++stats.rejectedGuarded;
			return 0;
		}

		commitGuardedRun(seg, i, run, vecs, packer);
		packer.coalesceSplats();
		stats.packedGuarded += n;
		++stats.guardedRuns;
		stats.guardPairs += (U32)packer.guardGroups.size();
		return total;
	}

	void SlpPackPass::Slp::commitGuardedRun(
			Segment& seg, U32 i, const List<WindowShape>& run, const List<Node*>& vecs, Packer& packer) {
		const WindowShape& w0 = run[0];
		U32 n = (U32)run.size();
		U32 total = n * w0.w;
		Node* wPtr = w0.byOff[0]->store->getPointer();
		StoreNode* lastInChain = seg[i + total - 1].store;
		Type* ctrlTy = fn.ctrlTy();
		Node* runBytes = fn.create<ConstantNode>(fn.types().getInt(64), (I64)(n * kVecBytes));
		Node* wEnd = fn.create<BinaryNode>(Opcode::Add, wPtr->getType(), wPtr, runBytes);

		// guard cascade: speculate in the then-arm, keep the scalar path in the else-arm
		Node* iff = nullptr;
		List<Node*> fails;
		Node* thenP = emitGuardCascade(packer.guardGroups, w0.ctrl, wPtr, wEnd, iff, fails);
		Node* elseP = nullptr;
		if(fails.size() == 1)
			elseP = fails[0];
		else
			elseP = fn.create<RegionNode>(ctrlTy, fails);

		// sink the speculated loads into the then-arm
		for(const Node* c : packer.madeLoads) {
			LoadNode* ld = cast<LoadNode>(const_cast<Node*>(c));
			if(ld->getControl() == w0.ctrl)
				ld->setInput(0, thenP);
		}

		// wide stores chain down the then-arm
		Node* prevMem = packer.memIn;
		for(U32 k = 0; k < n; ++k) {
			const StoreInfo* lane0 = run[k].byOff[0];
			Node* ptr = packer.anchorPtr(lane0->store->getPointer(), lane0->key);
			prevMem = fn.create<StoreNode>(fn.memTy(), thenP, prevMem, ptr, vecs[k]);
		}
		Node* region = fn.create<RegionNode>(ctrlTy, List<Node*>{thenP, elseP});

		// post-run consumers of the final state read the merged phi
		List<Node*> lastUsers;
		for(Node* u : lastInChain->getUsers())
			lastUsers.push_back(u);
		PhiNode* phi = fn.create<PhiNode>(fn.memTy(), List<Node*>{region, prevMem, lastInChain});
		for(Node* u : lastUsers)
			rewriteInput(u, lastInChain, phi);

		// scalar chain moves to the else arm
		for(U32 j = 0; j < total; ++j)
			seg[i + j].store->setInput(0, elseP);

		rerouteBlock(w0.ctrl, iff, region, elseP, packer);
	}

	Node* SlpPackPass::Slp::guardBranch(Node* ctrl, Node* pred) {
		Type* ctrlTy = fn.ctrlTy();
		Type* iffTy = fn.types().getTuple({ctrlTy, ctrlTy});
		return fn.create<IfNode>(iffTy, ctrl, pred);
	}

	Node* SlpPackPass::Slp::guardProj(Node* of, U32 index, const C8* label) {
		return fn.create<ProjNode>(fn.ctrlTy(), of, index, label);
	}

	Node* SlpPackPass::Slp::emitGuardCascade(const List<Packer::GuardGroup>& groups,
																					 Node* startCtrl,
																					 Node* wPtr,
																					 Node* wEnd,
																					 Node*& iff,
																					 List<Node*>& fails) {
		Type* i64 = fn.types().getInt(64);
		Type* boolTy = fn.boolTy();
		Type* ctrlTy = fn.ctrlTy();
		iff = nullptr;
		Node* ctrl = startCtrl;
		for(const auto& g : groups) {
			Node* gEnd = fn.create<BinaryNode>(
					Opcode::Add, g.ptr->getType(), g.ptr, fn.create<ConstantNode>(i64, g.maxC - g.minC));
			Node* bIf = guardBranch(ctrl, fn.create<CompareNode>(Opcode::Ule, boolTy, gEnd, wPtr));
			Node* bT = guardProj(bIf, IfNode::thenProjIndex(), "slp.then");
			Node* bF = guardProj(bIf, IfNode::elseProjIndex(), "slp.chk");
			Node* aIf = guardBranch(bF, fn.create<CompareNode>(Opcode::Ule, boolTy, wEnd, g.ptr));
			Node* aT = guardProj(aIf, IfNode::thenProjIndex(), "slp.then");
			fails.push_back(guardProj(aIf, IfNode::elseProjIndex(), "slp.else"));
			if(!iff)
				iff = bIf;
			ctrl = fn.create<RegionNode>(ctrlTy, List<Node*>{bT, aT});
		}
		if(!iff) { // observers but no may-alias load groups
			iff = guardBranch(startCtrl, fn.create<ConstantNode>(boolTy, 1));
			ctrl = guardProj(iff, IfNode::thenProjIndex(), "slp.then");
			fails.push_back(guardProj(iff, IfNode::elseProjIndex(), "slp.else"));
		}
		return ctrl;
	}

	Set<const Node*> SlpPackPass::Slp::preStates(Node* memIn) {
		Set<const Node*> pre;
		Node* m = memIn;
		U32 hop = 0;
		while(m && hop++ < 4096) {
			pre.insert(m);
			if(m->getOpcode() == Opcode::Store)
				m = cast<StoreNode>(m)->getMemory();
			else if(ProjNode* p = dyn_cast<ProjNode>(m)) {
				Node* prod = p->getProducer();
				if(prod->getOpcode() == Opcode::Call)
					m = cast<CallNode>(prod)->getMemory();
				else if(prod->getOpcode() == Opcode::Asm)
					m = cast<AsmNode>(prod)->getMemory();
				else
					break; // start (or foreign): done
			} else {
				break; // phi or other merge: done
			}
		}
		return pre;
	}

	// the wide stores hang off the then-arm and the scalar stores were moved to the
	// else-arm already, so the only user left to skip is the first guard branch
	void SlpPackPass::Slp::rerouteBlock(
			Node* startCtrl, Node* iff, Node* region, Node* elseP, const Packer& packer) {
		Set<const Node*> preSet = preStates(packer.memIn);
		List<Node*> ctrlUsers;
		for(Node* u : startCtrl->getUsers())
			ctrlUsers.push_back(u);
		for(Node* u : ctrlUsers) {
			if(u == iff)
				continue;
			Node* dest = nullptr; // null: keep the pre-branch anchor
			if(isControlNode(u)) {
				dest = region; // terminator or successor-region edge
			} else {
				Node* m = nullptr;
				if(LoadNode* ld = dyn_cast<LoadNode>(u))
					m = ld->getMemory();
				else if(StoreNode* st = dyn_cast<StoreNode>(u))
					m = st->getMemory();
				else if(CallNode* cl = dyn_cast<CallNode>(u))
					m = cl->getMemory();
				else if(AsmNode* as = dyn_cast<AsmNode>(u))
					m = as->getMemory();
				if(m && packer.interWritten.count(m))
					dest = elseP;
				else if(m && !preSet.count(m))
					dest = region; // post-run
				else if(m && isa<LoadNode>(u) && coneEndsInStores(u, packer.runStores, 64))
					dest = elseP; // pre-run load only the scalar arm reads
			}
			if(dest)
				rewriteInput(u, startCtrl, dest);
		}
	}
} // namespace rat
