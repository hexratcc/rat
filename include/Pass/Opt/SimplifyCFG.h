#ifndef RAT_PASS_OPT_SIMPLIFYCFG_H
#define RAT_PASS_OPT_SIMPLIFYCFG_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;

	U32 simplifyCFG(Function& fn);

	struct SimplifyCFGPass : Pass {
		const char* name() const override;
		B32 run(Module& module) override;
	};
} // namespace rat

#endif
