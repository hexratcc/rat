#ifndef RAT_CC_COMPILE_H
#define RAT_CC_COMPILE_H

#include "core.h"

#include "target/target.h"

namespace rat {
	struct MachinePass;
	struct Module;
	struct Pass;
	struct PassManager;
	struct TargetInfo;
} // namespace rat

namespace rat::cc {
	struct CompileOptions {
		List<UniquePtr<Pass>> optPasses;
		List<UniquePtr<MachinePass>> machinePasses; // empty = default x86 pipeline
		String renameMain;
	};

	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out);
	void compileModule(Module& mod, const TargetInfo& target, CompileOptions& opt, std::ostream& out);
} // namespace rat::cc

#endif
