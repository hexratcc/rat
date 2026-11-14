#include "pass/opt/simplify_cfg.h"

#include "codegen/schedule.h"
#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	void SimplifyCFGPass::reachableControl(Function& fn) {
		reach.assign(fn.idBound(), 0);
		reach[fn.getStart()->getId()] = 1;
		stack.clear();
		if(Node* e = fn.getStart()->projection(StartNode::controlProjIndex()))
			stack.push_back(e);
		while(!stack.empty()) {
			Node* n = stack.back();
			stack.pop_back();
			if(reach[n->getId()])
				continue;
			reach[n->getId()] = 1;
			for(Node* u : n->getUsers()) {
				if(isControlNode(u))
					stack.push_back(u);
				else if((isa<CallNode>(u) || isa<AsmNode>(u)) && u->getControlInput() == n)
					if(Node* cp = u->projection(CallNode::controlProjIndex()))
						stack.push_back(cp);
			}
		}
	}

	B32 SimplifyCFGPass::reached(Node* n) const {
		return n && n->getId() < reach.size() && reach[n->getId()];
	}

	void SimplifyCFGPass::removePred(RegionNode* r, U32 i) {
		for(PhiNode* p : usersOfType<PhiNode>(r))
			p->removeInput(1 + i);
		r->removeInput(i);
	}

	// keep the taken arm, cut the other out of the regions it reaches
	void SimplifyCFGPass::foldIf(Function& fn, IfNode* iff, B32 thenTaken) {
		ProjNode* taken = iff->projection(IfNode::thenProjIndex());
		ProjNode* dead = iff->projection(IfNode::elseProjIndex());
		if(!thenTaken)
			std::swap(taken, dead);
		if(taken)
			taken->replaceAllUsesWith(iff->getControl());
		for(U32 k = 0; dead && k < dead->getUsers().size();) {
			RegionNode* r = dyn_cast<RegionNode>(dead->getUsers()[k]);
			if(!r) {
				++k;
				continue;
			}
			for(U32 i = r->getPredecessorCount(); i-- > 0;)
				if(r->getPredecessor(i) == dead)
					removePred(r, i);
		}
		fn.removeNode(iff);
		if(taken)
			fn.removeNode(taken);
		if(dead)
			fn.removeNode(dead);
	}

	// already materialized, or anchored to control this rewrite does not touch
	B32 SimplifyCFGPass::freeValue(Node* v) {
		if(isa<ConstantNode>(v) || isa<GlobalNode>(v) || isa<AllocNode>(v) || isa<ProjNode>(v) ||
			 isa<PhiNode>(v))
			return true;
		return !v->hasSideEffects() && v->getControlInput() != nullptr;
	}

	B32 SimplifyCFGPass::cheapOp(Opcode op) {
		switch(op) {
		case Opcode::Add:
		case Opcode::Sub:
		case Opcode::And:
		case Opcode::Or:
		case Opcode::Xor:
		case Opcode::Shl:
		case Opcode::LShr:
		case Opcode::AShr:
		case Opcode::Neg:
		case Opcode::Not:
		case Opcode::Trunc:
		case Opcode::SExt:
		case Opcode::ZExt:
			return true;
		default:
			return isCompareOpcode(op) && op < Opcode::FEq;
		}
	}

	B32 SimplifyCFGPass::walkCone(Node* root) {
		B32 trap = false;
		stack.clear();
		stack.push_back(root);
		while(!stack.empty()) {
			Node* n = stack.back();
			stack.pop_back();
			if(!coneSeen.insert(n).second || freeValue(n))
				continue;
			trap |= Schedule::mayTrap(n);
			for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
				if(Node* in = n->getInput(i))
					stack.push_back(in);
		}
		return trap;
	}

	B32 SimplifyCFGPass::coneMayTrap(Node* v, Node* pred) {
		coneSeen.clear();
		walkCone(pred);
		return walkCone(v);
	}

	I32 SimplifyCFGPass::speculationCost(Node* v, Node* phi, U32 depth) {
		if(freeValue(v))
			return 0;
		if(depth == 0 || !cheapOp(v->getOpcode()))
			return -1;
		for(Node* u : v->getUsers())
			if(u != phi)
				return 0;
		I32 total = 1;
		for(U32 i = 0, e = v->getInputCount(); i < e; ++i) {
			Node* in = v->getInput(i);
			if(!in)
				return -1;
			I32 c = speculationCost(in, phi, depth - 1);
			if(c < 0)
				return -1;
			total += c;
		}
		return total;
	}

	// try to rewrite one empty-armed diamond as selects
	B32 SimplifyCFGPass::regionToSelect(Function& fn, RegionNode* r) {
		if(r->isLoopHeader() || r->getPredecessorCount() != 2)
			return false;
		ProjNode* a = dyn_cast<ProjNode>(r->getPredecessor(0));
		ProjNode* b = dyn_cast<ProjNode>(r->getPredecessor(1));
		if(!a || !b)
			return false;
		IfNode* iff = dyn_cast<IfNode>(a->getProducer());
		if(!iff || iff != dyn_cast<IfNode>(b->getProducer()))
			return false;
		// nothing may be anchored to the projections
		if(a->getUsers().size() != 1 || b->getUsers().size() != 1 || a->getIndex() == b->getIndex())
			return false;

		Node* pred = iff->getPredicate();
		// predecessor slot carrying the then-edge
		U32 thenSlot = a->getIndex() == IfNode::thenProjIndex() ? 0u : 1u;

		List<PhiNode*> phis = usersOfType<PhiNode>(r);
		I32 cost = 0;
		for(PhiNode* phi : phis) {
			if(phi->getValueCount() != 2)
				return false;
			Node* tv = phi->getValue(thenSlot);
			Node* fv = phi->getValue(1 - thenSlot);
			if(tv == fv)
				continue; // degenerate
			I32 c0 = speculationCost(tv, phi, kSpeculationDepth);
			I32 c1 = speculationCost(fv, phi, kSpeculationDepth);
			Type* t = phi->getType();
			if(!t || !(t->isInt() || t->isPtr()) || c0 < 0 || c1 < 0)
				return false;
			if(coneMayTrap(tv, pred) || coneMayTrap(fv, pred))
				return false;
			cost += c0 + c1;
		}
		if(phis.empty() || cost > kSpeculationBudget)
			return false;

		for(PhiNode* phi : phis) {
			Node* merged = phi->getValue(0);
			Node* elseValue = phi->getValue(1 - thenSlot);
			if(merged != phi->getValue(1))
				merged = fn.create<SelectNode>(phi->getType(), pred, phi->getValue(thenSlot), elseValue);
			phi->replaceAllUsesWith(merged);
		}
		// splice the merge out of the control chain, then drop the diamond
		r->replaceAllUsesWith(iff->getControl());
		if(DebugBody* dbg = fn.getDebug()) {
			dbg->mergeArm(a, iff->getControl());
			dbg->mergeArm(b, iff->getControl());
		}
		for(PhiNode* phi : phis)
			if(!phi->hasUsers())
				fn.removeNode(phi);
		fn.removeNode(r);
		fn.removeNode(a);
		fn.removeNode(b);
		fn.removeNode(iff);
		return true;
	}

	// keeps the ifs it did not fold
	U32 SimplifyCFGPass::foldConstantIfs(Function& fn) {
		U32 kept = 0;
		for(IfNode* iff : ifs) {
			if(ConstantNode* c = dyn_cast<ConstantNode>(iff->getPredicate()))
				foldIf(fn, iff, c->getValue() != 0);
			else
				ifs[kept++] = iff;
		}
		U32 folded = (U32)ifs.size() - kept;
		ifs.resize(kept);
		return folded;
	}

	// detach unreachable control and everything anchored to it
	U32 SimplifyCFGPass::clearUnreachable(Function& fn) {
		U32 changed = 0;
		if(StopNode* stop = fn.getStop())
			for(U32 i = stop->getInputCount(); i-- > 0;) {
				Node* r = stop->getInput(i);
				if(r && !reached(r)) {
					stop->removeInput(i);
					++changed;
				}
			}
		for(Node* n : fn) {
			if(n == fn.getStart() || n == fn.getStop())
				continue;
			B32 dead = false;
			if(isControlNode(n))
				dead = !reached(n);
			else if(Node* ci = n->getControlInput())
				dead = ci != fn.getStart() && !reached(ci);
			if(dead && n->getInputCount() > 0) {
				n->clearInputs();
				++changed;
			}
		}
		return changed;
	}

	U32 SimplifyCFGPass::dropDeadPreds() {
		U32 changed = 0;
		for(RegionNode* r : regions) {
			if(!reached(r))
				continue;
			for(U32 i = r->getPredecessorCount(); i-- > 0;)
				if(!reached(r->getPredecessor(i))) {
					removePred(r, i);
					++changed;
				}
		}
		return changed;
	}

	// an if with one used arm is straight-line control
	U32 SimplifyCFGPass::foldDegenerateIfs(Function& fn) {
		U32 changed = 0;
		for(IfNode* iff : ifs) {
			ProjNode* thenP = iff->projection(IfNode::thenProjIndex());
			ProjNode* elseP = iff->projection(IfNode::elseProjIndex());
			B32 thenLive = thenP && thenP->hasUsers();
			B32 elseLive = elseP && elseP->hasUsers();
			if(thenLive == elseLive)
				continue;
			foldIf(fn, iff, thenLive);
			++changed;
		}
		return changed;
	}

	U32 SimplifyCFGPass::collapseRegions() {
		U32 changed = 0;
		for(RegionNode* r : regions) {
			if(!reached(r) || r->getPredecessorCount() != 1)
				continue;
			for(PhiNode* phi : usersOfType<PhiNode>(r))
				phi->replaceAllUsesWith(phi->getValue(0));
			r->replaceAllUsesWith(r->getPredecessor(0));
			++changed;
		}
		return changed;
	}

	const C8* SimplifyCFGPass::name() const { return "simplifycfg"; }

	U32 SimplifyCFGPass::runOnFunction(Function& fn, const TargetInfo&) {
		U32 changed = 0;
		while(true) {
			ifs.clear();
			regions.clear();
			for(Node* n : fn) {
				if(IfNode* iff = dyn_cast<IfNode>(n))
					ifs.push_back(iff);
				else if(RegionNode* r = dyn_cast<RegionNode>(n))
					regions.push_back(r);
			}
			U32 sweep = foldConstantIfs(fn);
			reachableControl(fn);
			sweep += clearUnreachable(fn);
			sweep += dropDeadPreds();
			sweep += foldDegenerateIfs(fn);
			sweep += collapseRegions();
			for(RegionNode* r : regions)
				sweep += regionToSelect(fn, r);
			fn.eliminateDeadNodes(true);
			changed += sweep;
			if(!sweep)
				return changed;
		}
	}
} // namespace rat
