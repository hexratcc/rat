#ifndef RAT_PASS_OPT_SIMPLIFYCFG_H
#define RAT_PASS_OPT_SIMPLIFYCFG_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;

	U32 simplifyCFG(Function& fn);

	struct SimplifyCFGPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn) override;
	};
} // namespace rat

#endif
