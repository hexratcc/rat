#ifndef RAT_CC_COMPILE_H
#define RAT_CC_COMPILE_H

#include "core.h"

#include <iosfwd>

namespace rat {
	struct MachinePass;
	struct Pass;
	struct PassManager;
} // namespace rat

namespace rat::cc {
	struct CompileOptions {
		List<UniquePtr<Pass>> optPasses;
		List<UniquePtr<MachinePass>> machinePasses; // empty = default x86 pipeline
		B32 optimize = false;												// default pipeline runs the post-RA cleanups
	};

	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out);
} // namespace rat::cc

#endif
