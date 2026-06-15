#ifndef RAT_PASS_OPT_GVN_H
#define RAT_PASS_OPT_GVN_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;

	U32 gvn(Function& fn);

	struct GVNPass : Pass {
		const char* name() const override;
		B32 run(Module& module) override;
	};
} // namespace rat

#endif
