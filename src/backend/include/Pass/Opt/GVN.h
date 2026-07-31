// global value numbering: hash-cons congruent nodes so equal computations
// share a single node
//
// references:
// - B. Alpern, M. Wegman and F. K. Zadeck, "Detecting Equality of Variables
//   in Programs", POPL, 1988
// - C. Click, "Global Code Motion / Global Value Numbering", PLDI, 1995

#ifndef RAT_PASS_OPT_GVN_H
#define RAT_PASS_OPT_GVN_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;

	struct GVNPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		static B32 isPureValue(Node* n);
	};
} // namespace rat

#endif
