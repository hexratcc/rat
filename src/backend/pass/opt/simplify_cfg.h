// control-flow simplification: fold branches on constant predicates, collapse
// single-predecessor regions and their phis, and prune unreachable control
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

	struct SimplifyCFGPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		void reachableControl(Function& fn);
		void collectPhis(Node* region, List<PhiNode*>& out);
		void detachFromRegions(Node* ctrl);

		Set<Node*> reach;
		List<Node*> work;
		List<Node*> ifs;
		List<Node*> regions;
		List<Node*> regionUsers;
		List<PhiNode*> phis;
		List<PhiNode*> detachPhis;
	};
} // namespace rat

#endif
