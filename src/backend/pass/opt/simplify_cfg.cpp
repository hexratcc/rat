#include "pass/opt/simplify_cfg.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	void SimplifyCFGPass::reachableControl(Function& fn) {
		reach.clear();
		reach.insert(fn.getStart());
		work.clear();
		if(Node* e = fn.getStart()->projection(StartNode::controlProjIndex()))
			work.push_back(e);
		while(!work.empty()) {
			Node* n = work.back();
			work.pop_back();
			if(!reach.insert(n).second)
				continue;
			for(Node* u : n->getUsers()) {
				if(isControlNode(u)) {
					work.push_back(u);
				} else if(isa<CallNode>(u) || isa<AsmNode>(u)) {
					if(u->getControlInput() == n)
						if(Node* cp = u->projection(CallNode::controlProjIndex()))
							work.push_back(cp);
				}
			}
		}
	}

	void SimplifyCFGPass::collectPhis(Node* region, List<PhiNode*>& out) {
		out.clear();
		for(Node* u : region->getUsers())
			if(PhiNode* p = dyn_cast<PhiNode>(u))
				out.push_back(p);
	}

	void SimplifyCFGPass::detachFromRegions(Node* ctrl) {
		regionUsers.clear();
		for(Node* u : ctrl->getUsers())
			if(isa<RegionNode>(u))
				regionUsers.push_back(u);
		for(Node* n : regionUsers) {
			RegionNode* r = cast<RegionNode>(n);
			collectPhis(r, detachPhis);
			for(I32 i = (I32)r->getPredecessorCount() - 1; i >= 0; --i) {
				if(r->getPredecessor(i) != ctrl)
					continue;
				for(PhiNode* phi : detachPhis)
					phi->removeInput(1 + i);
				r->removeInput(i);
			}
		}
	}

	// already materialized, or anchored to control this rewrite does not touch
	B32 SimplifyCFGPass::freeValue(Node* v) {
		if(isa<ConstantNode>(v) || isa<GlobalNode>(v) || isa<AllocNode>(v) || isa<ProjNode>(v) ||
			 isa<PhiNode>(v))
			return true;
		if(v->hasSideEffects())
			return false;
		return v->getControlInput() != nullptr;
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

	B32 SimplifyCFGPass::selectableType(Type* t) { return t && (t->isInt() || t->isPtr()); }

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
		if(a->getUsers().size() != 1 || b->getUsers().size() != 1)
			return false;
		if(a->getIndex() == b->getIndex())
			return false;

		Node* pred = iff->getPredicate();
		// predecessor slot carrying the then-edge
		U32 thenSlot = a->getIndex() == IfNode::thenProjIndex() ? 0u : 1u;

		collectPhis(r, phis);
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
			if(!selectableType(phi->getType()) || c0 < 0 || c1 < 0)
				return false;
			cost += c0 + c1;
		}
		if(phis.empty() || cost > kSpeculationBudget)
			return false;

		for(PhiNode* phi : phis) {
			if(phi->getValue(0) == phi->getValue(1)) {
				phi->replaceAllUsesWith(phi->getValue(0));
			} else {
				Node* sel = fn.create<SelectNode>(
						phi->getType(), pred, phi->getValue(thenSlot), phi->getValue(1 - thenSlot));
				phi->replaceAllUsesWith(sel);
			}
		}
		// splice the merge out of the control chain, then drop the diamond
		r->replaceAllUsesWith(iff->getControl());
		auto drop = [&](Node* dead) {
			if(dead && !dead->hasUsers())
				fn.removeNode(dead);
		};
		for(PhiNode* phi : phis)
			drop(phi);
		drop(r);
		drop(a);
		drop(b);
		drop(iff);
		return true;
	}

	U32 SimplifyCFGPass::ifToSelect(Function& fn) {
		selectRegions.clear();
		for(Node* n : fn)
			if(RegionNode* r = dyn_cast<RegionNode>(n))
				selectRegions.push_back(r);
		U32 changed = 0;
		for(Node* n : selectRegions)
			changed += regionToSelect(fn, cast<RegionNode>(n));
		return changed;
	}

	const C8* SimplifyCFGPass::name() const { return "simplifycfg"; }

	U32 SimplifyCFGPass::runOnFunction(Function& fn, const TargetInfo&) {
		U32 changed = 0;
		B32 again = true;
		while(again) {
			again = false;

			ifs.clear();
			regions.clear();
			for(Node* n : fn) {
				Opcode op = n->getOpcode();
				if(op == Opcode::If)
					ifs.push_back(n);
				else if(op == Opcode::Region)
					regions.push_back(n);
			}

			U32 live = 0;
			for(Node* n : ifs) {
				IfNode* iff = cast<IfNode>(n);
				Node* pred = iff->getPredicate();
				ConstantNode* c = dyn_cast<ConstantNode>(pred);
				if(!c) {
					ifs[live++] = n;
					continue;
				}
				U32 takenIdx = c->getValue() != 0 ? IfNode::thenProjIndex() : IfNode::elseProjIndex();
				U32 deadIdx =
						takenIdx == IfNode::thenProjIndex() ? IfNode::elseProjIndex() : IfNode::thenProjIndex();
				Node* ctrl = iff->getControl();
				ProjNode* taken = iff->projection(takenIdx);
				ProjNode* dead = iff->projection(deadIdx);
				if(taken)
					taken->replaceAllUsesWith(ctrl);
				if(dead)
					detachFromRegions(dead);
				fn.removeNode(iff);
				if(taken)
					fn.removeNode(taken);
				if(dead)
					fn.removeNode(dead);
				++changed;
				again = true;
			}
			ifs.resize(live);

			reachableControl(fn);

			if(StopNode* stop = fn.getStop()) {
				for(I32 i = (I32)stop->getInputCount() - 1; i >= 0; --i) {
					Node* r = stop->getInput((U32)i);
					if(r && !reach.count(r)) {
						stop->removeInput((U32)i);
						++changed;
						again = true;
					}
				}
			}

			for(Node* n : fn) {
				if(n == fn.getStart() || n == fn.getStop())
					continue;
				B32 dead = false;
				if(isControlNode(n))
					dead = !reach.count(n);
				else if(Node* ci = n->getControlInput())
					dead = ci != fn.getStart() && !reach.count(ci);
				if(!dead)
					continue;
				if(RegionNode* r = dyn_cast<RegionNode>(n)) {
					collectPhis(r, phis);
					for(PhiNode* phi : phis)
						if(phi->getInputCount() > 0) {
							phi->clearInputs();
							++changed;
							again = true;
						}
				}
				if(n->getInputCount() > 0) {
					n->clearInputs();
					++changed;
					again = true;
				}
			}

			// drop region predecessors whose control is no longer reachable
			for(Node* n : regions) {
				RegionNode* r = cast<RegionNode>(n);
				if(!reach.count(r))
					continue;
				collectPhis(r, phis);
				for(I32 i = (I32)r->getPredecessorCount() - 1; i >= 0; --i) {
					if(reach.count(r->getPredecessor(i)))
						continue;
					for(PhiNode* phi : phis)
						phi->removeInput(1 + i);
					r->removeInput(i);
					++changed;
					again = true;
				}
			}

			// fold ifs whose successor structure has degenerated
			for(Node* n : ifs) {
				IfNode* iff = cast<IfNode>(n);
				ProjNode* thenP = iff->projection(IfNode::thenProjIndex());
				ProjNode* elseP = iff->projection(IfNode::elseProjIndex());
				B32 thenLive = thenP && thenP->hasUsers();
				B32 elseLive = elseP && elseP->hasUsers();
				if(thenLive == elseLive)
					continue;
				ProjNode* live = thenLive ? thenP : elseP;
				Node* ctrl = iff->getControl();
				live->replaceAllUsesWith(ctrl);
				fn.removeNode(iff);
				if(thenP)
					fn.removeNode(thenP);
				if(elseP)
					fn.removeNode(elseP);
				++changed;
				again = true;
			}

			// collapse single predecessor regions
			for(Node* n : regions) {
				RegionNode* r = cast<RegionNode>(n);
				if(!reach.count(r) || r->getPredecessorCount() != 1)
					continue;
				collectPhis(r, phis);
				for(PhiNode* phi : phis)
					phi->replaceAllUsesWith(phi->getValue(0));
				r->replaceAllUsesWith(r->getPredecessor(0));
				++changed;
				again = true;
			}

			if(U32 sel = ifToSelect(fn)) {
				changed += sel;
				again = true;
			}

			fn.eliminateDeadNodes(true);
		}
		return changed;
	}
} // namespace rat
