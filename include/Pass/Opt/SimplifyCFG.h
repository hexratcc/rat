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

#include "Core.h"
#include "IR/Opcode.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Node;

	struct SimplifyCFGPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn) override;
	private:
		Set<Node*> reachableControl(Function& fn);
		List<Node*> nodesOfOpcode(Function& fn, Opcode op);
	};
} // namespace rat

#endif
