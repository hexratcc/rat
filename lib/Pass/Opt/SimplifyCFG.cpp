// control-flow simplification: fold branches on constant predicates, collapse
// single-predecessor regions and their phis, and prune unreachable control
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995
// - C. Click, "Combining Analyses, Combining Optimizations", PhD thesis,
//   Rice University, 1995

#include "Pass/Opt/SimplifyCFG.h"

#include "IR/Function.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	namespace detail {
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

		Set<Node*> reachableControl(Function& fn) {
			Set<Node*> seen;
			seen.insert(fn.getStart());
			List<Node*> work;
			if (Node* e = fn.getStart()->projection(StartNode::controlProjIndex()))
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
				if (PhiNode* p = dyn_cast<PhiNode>(u))
					phis.push_back(p);
			return phis;
		}
	} // namespace detail
	using namespace detail;

	U32 simplifyCFG(Function& fn) {
		U32 changed = 0;
		B32 again = true;
		while (again) {
			again = false;

			// constant branch folding
			for (Node* n : nodesOfOpcode(fn, Opcode::If)) {
				IfNode* iff = cast<IfNode>(n);
				Node* pred = iff->getPredicate();
				ConstantNode* c = dyn_cast<ConstantNode>(pred);
				if (!c)
					continue;
				U32 takenIdx = c->getValue() != 0 ? IfNode::thenProjIndex()
																					: IfNode::elseProjIndex();
				Node* ctrl = iff->getControl();
				if (ProjNode* taken = iff->projection(takenIdx))
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
				RegionNode* r = cast<RegionNode>(n);
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
				RegionNode* r = cast<RegionNode>(n);
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

	const C8* SimplifyCFGPass::name() const { return "simplifycfg"; }

	U32 SimplifyCFGPass::runOnFunction(Function& fn) { return simplifyCFG(fn); }
} // namespace rat
