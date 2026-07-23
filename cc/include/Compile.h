#ifndef RAT_CC_COMPILE_H
#define RAT_CC_COMPILE_H

#include "Core.h"

#include "Target/Target.h"

namespace rat {
	struct Module;
	struct Pass;
	struct PassManager;
	struct TargetInfo;
} // namespace rat

namespace rat::cc {
	enum struct Backend { C, X86 };

	struct CompileOptions {
		Backend backend = Backend::X86;
		List<UniquePtr<Pass>> optPasses;
		String renameMain;
	};

	List<UniquePtr<Pass>> defaultOptPasses();
	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out);
	void compileModule(Module& mod, const TargetInfo& target, CompileOptions& opt, std::ostream& out);
} // namespace rat::cc

#endif
