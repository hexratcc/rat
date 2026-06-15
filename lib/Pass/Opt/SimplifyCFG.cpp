#include "Pass/Opt/SimplifyCFG.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

#include <unordered_set>

namespace rat {
	namespace {
		B32 isControlNode(Node* n) {
			switch (n->getOpcode()) {
			case Opcode::Start:
			case Opcode::Stop:
			case Opcode::Return:
			case Opcode::Region:
			case Opcode::If:
				return true;
			case Opcode::Proj:
				return n->getType()->isControl();
			default:
				return false;
			}
		}

		ProjNode* asProj(Node* n) {
			return n->getOpcode() == Opcode::Proj ? static_cast<ProjNode*>(n)
																						: nullptr;
		}

		Node* entryControl(Function& fn) {
			for (Node* u : fn.getStart()->getUsers())
				if (ProjNode* p = asProj(u))
					if (p->getIndex() == StartNode::controlProjIndex())
						return p;
			return nullptr;
		}

		ProjNode* projOf(IfNode* iff, U32 index) {
			for (Node* u : iff->getUsers())
				if (ProjNode* p = asProj(u))
					if (p->getIndex() == index)
						return p;
			return nullptr;
		}

		Set<Node*> reachableControl(Function& fn) {
			Set<Node*> seen;
			seen.insert(fn.getStart());
			List<Node*> work;
			if (Node* e = entryControl(fn))
				work.push_back(e);
			while (!work.empty()) {
				Node* n = work.back();
				work.pop_back();
				if (!seen.insert(n).second)
					continue;
				for (Node* u : n->getUsers())
					if (isControlNode(u))
						work.push_back(u);
			}
			return seen;
		}

		List<Node*> nodesOfOpcode(Function& fn, Opcode op) {
			List<Node*> out;
			for (Node* n : fn)
				if (n->getOpcode() == op)
					out.push_back(n);
			return out;
		}

		List<PhiNode*> phisOn(RegionNode* r) {
			List<PhiNode*> phis;
			for (Node* u : r->getUsers())
				if (u->getOpcode() == Opcode::Phi)
					phis.push_back(static_cast<PhiNode*>(u));
			return phis;
		}
	} // namespace

	U32 simplifyCFG(Function& fn) {
		U32 changed = 0;
		B32 again = true;
		while (again) {
			again = false;

			// constant branch folding
			for (Node* n : nodesOfOpcode(fn, Opcode::If)) {
				IfNode* iff = static_cast<IfNode*>(n);
				Node* pred = iff->getPredicate();
				if (pred->getOpcode() != Opcode::Constant)
					continue;
				ConstantNode* c = static_cast<ConstantNode*>(pred);
				U32 takenIdx = c->getValue() != 0 ? IfNode::thenProjIndex()
																					: IfNode::elseProjIndex();
				Node* ctrl = iff->getControl();
				if (ProjNode* taken = projOf(iff, takenIdx))
					taken->replaceAllUsesWith(ctrl);
				while (iff->getInputCount() > 0)
					iff->removeInput(iff->getInputCount() - 1);
				++changed;
				again = true;
			}

			auto reach = reachableControl(fn);
			auto regions = nodesOfOpcode(fn, Opcode::Region);

			// drop region predecessors whose control is no longer reachable
			for (Node* n : regions) {
				RegionNode* r = static_cast<RegionNode*>(n);
				if (!reach.count(r))
					continue;
				List<PhiNode*> phis = phisOn(r);
				for (I32 i = (I32)r->getPredecessorCount() - 1; i >= 0; --i) {
					if (reach.count(r->getPredecessor(i)))
						continue;
					for (PhiNode* phi : phis)
						phi->removeInput(1 + i);
					r->removeInput(i);
					++changed;
					again = true;
				}
			}

			// collapse single predecessor regions
			for (Node* n : regions) {
				RegionNode* r = static_cast<RegionNode*>(n);
				if (!reach.count(r) || r->getPredecessorCount() != 1)
					continue;
				for (PhiNode* phi : phisOn(r))
					phi->replaceAllUsesWith(phi->getValue(0));
				r->replaceAllUsesWith(r->getPredecessor(0));
				++changed;
				again = true;
			}

			fn.eliminateDeadNodes(true);
		}
		return changed;
	}

	const char* SimplifyCFGPass::name() const { return "simplifycfg"; }

	B32 SimplifyCFGPass::run(Module& module) {
		U32 changed = 0;
		for (Function* fn : module)
			changed += simplifyCFG(*fn);
		return changed != 0;
	}
} // namespace rat
