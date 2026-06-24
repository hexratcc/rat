#ifndef RAT_PASS_OPT_INLINE_H
#define RAT_PASS_OPT_INLINE_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Module;

	U32 inlineCalls(Module& module);

	struct InlinePass : Pass {
		const C8* name() const override;
		B32 run(Module& module) override;
	};
} // namespace rat

#endif
