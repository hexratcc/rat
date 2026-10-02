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
	struct IfNode;
	struct Node;
	struct PhiNode;
	struct RegionNode;

	struct SimplifyCFGPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		// speculating too much undoes the branch it replaces, so a diamond only
		// converts while the work moved onto both paths stays under this budget
		static constexpr I32 kSpeculationBudget = 4;
		static constexpr U32 kSpeculationDepth = 4;

		U32 foldConstantIfs(Function& fn);
		U32 clearUnreachable(Function& fn);
		U32 dropDeadPreds();
		U32 foldDegenerateIfs(Function& fn);
		U32 collapseRegions();

		void reachableControl(Function& fn);
		B32 reached(Node* n) const;
		void collectPhis(Node* region);
		static void removePred(RegionNode* r, U32 i);
		static void foldIf(Function& fn, IfNode* iff, B32 thenTaken);

		// if-conversion
		static B32 freeValue(Node* v);
		static B32 cheapOp(Opcode op);
		B32 walkCone(Node* root);
		B32 coneMayTrap(Node* v, Node* pred);
		static I32 speculationCost(Node* v, Node* phi, U32 depth);
		B32 regionToSelect(Function& fn, RegionNode* r);
	private:
		List<Node*> ifs;
		List<Node*> regions;
		List<U8> reach; // id -> reachable control
		List<PhiNode*> phis;
		// walk scratch
		List<Node*> stack;
		Set<Node*> coneSeen;
	};
} // namespace rat

#endif
