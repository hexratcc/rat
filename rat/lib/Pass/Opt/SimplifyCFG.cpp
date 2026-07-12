#include "Pass/Opt/SimplifyCFG.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	Set<Node*> SimplifyCFGPass::reachableControl(Function& fn) {
		Set<Node*> seen;
		seen.insert(fn.getStart());
		List<Node*> work;
		if(Node* e = fn.getStart()->projection(StartNode::controlProjIndex()))
			work.push_back(e);
		while(!work.empty()) {
			Node* n = work.back();
			work.pop_back();
			if(!seen.insert(n).second)
				continue;
			for(Node* u : n->getUsers()) {
				if(isControlNode(u)) {
					work.push_back(u);
				} else if(CallNode* call = dyn_cast<CallNode>(u)) {
					if(call->getControlInput() == n)
						if(Node* cp = call->projection(CallNode::controlProjIndex()))
							work.push_back(cp);
				}
			}
		}
		return seen;
	}

	List<Node*> SimplifyCFGPass::nodesOfOpcode(Function& fn, Opcode op) {
		List<Node*> out;
		for(Node* n : fn)
			if(n->getOpcode() == op)
				out.push_back(n);
		return out;
	}

	void SimplifyCFGPass::detachFromRegions(Node* ctrl) {
		List<Node*> regionUsers;
		for(Node* u : ctrl->getUsers())
			if(isa<RegionNode>(u))
				regionUsers.push_back(u);
		for(Node* n : regionUsers) {
			RegionNode* r = cast<RegionNode>(n);
			List<PhiNode*> phis = usersOfType<PhiNode>(r);
			for(I32 i = (I32)r->getPredecessorCount() - 1; i >= 0; --i) {
				if(r->getPredecessor(i) != ctrl)
					continue;
				for(PhiNode* phi : phis)
					phi->removeInput(1 + i);
				r->removeInput(i);
			}
		}
	}

	const C8* SimplifyCFGPass::name() const { return "simplifycfg"; }

	U32 SimplifyCFGPass::runOnFunction(Function& fn) {
		U32 changed = 0;
		B32 again = true;
		while(again) {
			again = false;

			for(Node* n : nodesOfOpcode(fn, Opcode::If)) {
				IfNode* iff = cast<IfNode>(n);
				Node* pred = iff->getPredicate();
				ConstantNode* c = dyn_cast<ConstantNode>(pred);
				if(!c)
					continue;
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

			auto reach = reachableControl(fn);

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
				if(RegionNode* r = dyn_cast<RegionNode>(n))
					for(PhiNode* phi : usersOfType<PhiNode>(r))
						if(phi->getInputCount() > 0) {
							phi->clearInputs();
							++changed;
							again = true;
						}
				if(n->getInputCount() > 0) {
					n->clearInputs();
					++changed;
					again = true;
				}
			}

			auto regions = nodesOfOpcode(fn, Opcode::Region);

			// drop region predecessors whose control is no longer reachable
			for(Node* n : regions) {
				RegionNode* r = cast<RegionNode>(n);
				if(!reach.count(r))
					continue;
				List<PhiNode*> phis = usersOfType<PhiNode>(r);
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
			for(Node* n : nodesOfOpcode(fn, Opcode::If)) {
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
				for(PhiNode* phi : usersOfType<PhiNode>(r))
					phi->replaceAllUsesWith(phi->getValue(0));
				r->replaceAllUsesWith(r->getPredecessor(0));
				++changed;
				again = true;
			}

			fn.eliminateDeadNodes(true);
		}
		return changed;
	}
} // namespace rat
