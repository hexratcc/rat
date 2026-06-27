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

	struct GVNPass : Pass {
		const C8* name() const override;
		B32 run(Module& module) override;

		static B32 isPureValue(Node* n);
	private:
		U32 runOnFunction(Function& fn);
	};
} // namespace rat

#endif
