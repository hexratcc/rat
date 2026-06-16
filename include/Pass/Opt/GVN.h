#ifndef RAT_PASS_OPT_GVN_H
#define RAT_PASS_OPT_GVN_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;

	U32 gvn(Function& fn);

	struct GVNPass : FunctionPass {
		const char* name() const override;
		U32 runOnFunction(Function& fn) override;
	};
} // namespace rat

#endif
