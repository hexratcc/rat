// control-flow simplification: fold branches on constant predicates, collapse
// single-predecessor regions and their phis, prune unreachable control, and
// turn empty-armed diamonds into branch-free Select nodes
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995
// - C. Click, "Combining Analyses, Combining Optimizations", PhD thesis,
//   Rice University, 1995

#ifndef RAT_PASS_OPT_SIMPLIFYCFG_H
#define RAT_PASS_OPT_SIMPLIFYCFG_H

#include "core.h"
#include "ir/opcode.h"
#include "pass/pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;
	struct PhiNode;
	struct RegionNode;
	struct Type;

	struct SimplifyCFGPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		// speculating too much undoes the branch it replaces, so a diamond only
		// converts while the work moved onto both paths stays under this budget
		static constexpr I32 kSpeculationBudget = 4;
		static constexpr U32 kSpeculationDepth = 4;

		void reachableControl(Function& fn);
		void collectPhis(Node* region, List<PhiNode*>& out);
		void detachFromRegions(Node* ctrl);

		// if-conversion
		static B32 freeValue(Node* v);
		static B32 cheapOp(Opcode op);
		static I32 speculationCost(Node* v, Node* phi, U32 depth);
		static B32 selectableType(Type* t);
		U32 ifToSelect(Function& fn);
		B32 regionToSelect(Function& fn, RegionNode* r);
	private:
		Set<Node*> reach;
		List<Node*> work;
		List<Node*> ifs;
		List<Node*> regions;
		List<Node*> regionUsers;
		List<Node*> selectRegions;
		List<PhiNode*> phis;
		List<PhiNode*> detachPhis;
	};
} // namespace rat

#endif
