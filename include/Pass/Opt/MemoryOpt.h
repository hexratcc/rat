#ifndef RAT_PASS_OPT_MEMORYOPT_H
#define RAT_PASS_OPT_MEMORYOPT_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;

	U32 optimizeMemory(Function& fn);

	struct MemoryOptPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn) override;
	};
} // namespace rat

#endif
