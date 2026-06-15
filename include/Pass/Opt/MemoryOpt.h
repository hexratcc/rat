#ifndef RAT_PASS_OPT_MEMORYOPT_H
#define RAT_PASS_OPT_MEMORYOPT_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;

	U32 optimizeMemory(Function& fn);

	struct MemoryOptPass : Pass {
		const char* name() const override;
		B32 run(Module& module) override;
	};
} // namespace rat

#endif
