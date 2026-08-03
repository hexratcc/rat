#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "Core.h"

#include "Pass/Pass.h"

namespace rat {
	struct SlpPackPass : FunctionPass {
		const C8* name() const override { return "slp"; }
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	};
} // namespace rat

#endif
