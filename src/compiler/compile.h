#ifndef RAT_CC_COMPILE_H
#define RAT_CC_COMPILE_H

#include "core.h"

namespace rat {
	struct MachinePass;
	struct Pass;
	struct PassManager;
} // namespace rat

namespace rat::cc {
	struct CompileOptions {
		List<UniquePtr<Pass>> optPasses;
		List<UniquePtr<MachinePass>> machinePasses; // empty = default x86 pipeline
		String renameMain;
	};

	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out);
} // namespace rat::cc

#endif
