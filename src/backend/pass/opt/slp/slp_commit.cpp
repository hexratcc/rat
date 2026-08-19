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
		Map<const Node*, List<I64>> interWritten;
		Set<const Node*> obsSet;
		collectInterState(seg, i, w0.w, interWritten, obsSet);
		RefinedAddr wkey = w0.byOff[0]->key;
		Packer packer(fn, aa, ptrBytes, sse41, shapes, stats);
		packer.bindWindow(seg[i].store->getMemory(), &wkey, &interWritten, &obsSet, &addrAnchors);
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
			B32 sameShape = wn.ctrl == w0.ctrl && wn.esz == w0.esz;
			if(!sameGroup || !contiguous || !sameShape)
				break;
			run.push_back(std::move(wn));
		}
		U32 n = (U32)run.size();
		U32 total = n * w0.w;
		stats.windowsSeen += n - 1; // run extensions
		Node* memIn = seg[i].store->getMemory();
		Node* wPtr = w0.byOff[0]->store->getPointer();
		RefinedAddr wkey = w0.byOff[0]->key;
		Map<const Node*, List<I64>> interWritten;
		Set<const Node*> obsSet;
		collectInterState(seg, i, total, interWritten, obsSet);

		Packer packer(fn, aa, ptrBytes, sse41, shapes, stats);
		packer.bindWindow(memIn, &wkey, &interWritten, &obsSet, &addrAnchors);
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
		for(const auto& g : packer.guardGroups)
			accept = accept && !packer.coneTouchesObserver(g.ptr);
		if(accept)
			accept = observersConfined(seg, i, total, obsSet);
		if(!accept) {
			if(!treeOk)
				++stats.rejectedTree;
			else
				++stats.rejectedGuarded;
			return 0;
		}

		commitGuardedRun(seg, i, run, vecs, packer, interWritten);
		packer.coalesceSplats();
		stats.packedGuarded += n;
		++stats.guardedRuns;
		stats.guardPairs += (U32)packer.guardGroups.size();
		return total;
	}

	B32 SlpPackPass::Slp::observersConfined(const Segment& seg,
																					U32 i,
																					U32 total,
																					const Set<const Node*>& obsSet) const {
		Set<const Node*> runStores;
		for(U32 j = 0; j < total; ++j)
			runStores.insert(seg[i + j].store);
		for(const Node* obs : obsSet)
			if(!coneEndsInStores(obs, runStores, 128))
				return false;
		return true;
	}

	void SlpPackPass::Slp::commitGuardedRun(Segment& seg,
																					U32 i,
																					const List<WindowShape>& run,
																					const List<Node*>& vecs,
																					Packer& packer,
																					const Map<const Node*, List<I64>>& interWritten) {
		const WindowShape& w0 = run[0];
		U32 n = (U32)run.size();
		U32 total = n * w0.w;
		Node* memIn = seg[i].store->getMemory();
		Node* wPtr = w0.byOff[0]->store->getPointer();
		StoreNode* lastInChain = seg[i + total - 1].store;
		Set<const Node*> runStores;
		for(U32 j = 0; j < total; ++j)
			runStores.insert(seg[i + j].store);
		Type* ctrlTy = fn.ctrlTy();
		Node* wEnd =
				fn.create<BinaryNode>(Opcode::Add,
															wPtr->getType(),
															wPtr,
															fn.create<ConstantNode>(fn.types().getInt(64), (I64)(n * kVecBytes)));

		// guard cascade: speculate in the then-arm, keep the scalar path in the else-arm
		Node* iff = nullptr;
		List<Node*> fails;
		Node* thenP = emitGuardCascade(packer.guardGroups, w0.ctrl, wPtr, wEnd, iff, fails);
		Node* elseP = nullptr;
		if(fails.size() == 1)
			elseP = fails[0];
		else
			elseP = fn.create<RegionNode>(ctrlTy, fails);

		// wide stores chain down the then-arm
		Node* prevMem = memIn;
		Set<const Node*> wideStores;
		for(U32 k = 0; k < n; ++k) {
			Node* wide = fn.create<StoreNode>(
					fn.memTy(),
					thenP,
					prevMem,
					packer.anchorPtr(run[k].byOff[0]->store->getPointer(), run[k].byOff[0]->key),
					vecs[k]);
			wideStores.insert(wide);
			prevMem = wide;
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

		Set<const Node*> preSet;
		collectPreStates(memIn, preSet);
		rerouteBlock(w0.ctrl, iff, region, elseP, wideStores, runStores, preSet, interWritten);
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

	void SlpPackPass::Slp::collectPreStates(Node* memIn, Set<const Node*>& preSet) {
		Node* m = memIn;
		U32 hop = 0;
		while(m && hop++ < 4096) {
			preSet.insert(m);
			if(m->getOpcode() == Opcode::Store)
				m = cast<StoreNode>(m)->getMemory();
			else if(ProjNode* p = dyn_cast<ProjNode>(m)) {
				Node* prod = p->getProducer();
				if(prod->getOpcode() == Opcode::Call)
					m = cast<CallNode>(prod)->getMemory();
				else
					break; // start (or foreign): done
			} else {
				break; // phi or other merge: done
			}
		}
	}

	void SlpPackPass::Slp::rerouteBlock(Node* startCtrl,
																			Node* iff,
																			Node* region,
																			Node* elseP,
																			const Set<const Node*>& wideStores,
																			const Set<const Node*>& runStores,
																			const Set<const Node*>& preSet,
																			const Map<const Node*, List<I64>>& interWritten) {
		List<Node*> ctrlUsers;
		for(Node* u : startCtrl->getUsers())
			ctrlUsers.push_back(u);
		for(Node* u : ctrlUsers) {
			if(u == iff || wideStores.count(u) || runStores.count(u))
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
				if(m && interWritten.count(m))
					dest = elseP;
				else if(m && !preSet.count(m))
					dest = region; // post-run
				else if(m && isa<LoadNode>(u) && coneEndsInStores(u, runStores, 64))
					dest = elseP; // pre-run load only the scalar arm reads
			}
			if(dest)
				rewriteInput(u, startCtrl, dest);
		}
	}
} // namespace rat
