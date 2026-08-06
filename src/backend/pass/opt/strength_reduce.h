// loop induction-variable strength reduction

#ifndef RAT_PASS_OPT_STRENGTHREDUCE_H
#define RAT_PASS_OPT_STRENGTHREDUCE_H

#include "core.h"

#include "pass/pass.h"

namespace rat {
	struct StrengthReducePass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	};
} // namespace rat

#endif
