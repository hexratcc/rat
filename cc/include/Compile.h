#ifndef RAT_CC_COMPILE_H
#define RAT_CC_COMPILE_H

#include "Core.h"
#include <cstdio>

#include "Target/Target.h"

namespace rat {
	struct Module;
	struct Pass;
	struct PassManager;
	struct TargetInfo;
} // namespace rat

namespace rat::cc {
	enum struct Backend { C, X86 };
	enum struct RegAlloc { Linear, Graph };

	struct CompileOptions {
		Backend backend = Backend::X86;
		RegAlloc regAlloc = RegAlloc::Linear;
		List<UniquePtr<Pass>> optPasses;
		String renameMain;
	};

	const TargetTriple& hostTargetTriple();
	void setHostTargetTriple(const TargetTriple& triple);
	U32 targetLongBits(const TargetTriple& triple);
	const char* nullDevice();
	FILE* shellOpen(const char* cmd); // portable popen(cmd, "r")
	I32 shellClose(FILE* p);

	const String& hostCC();
	const String& hostPredefs();
	const List<String>& hostIncludeDirs();

	List<UniquePtr<Pass>> defaultOptPasses();
	void composePipeline(PassManager& pm, CompileOptions& opt, std::ostream& out);
	void compileModule(Module& mod, const TargetInfo& target, CompileOptions& opt, std::ostream& out);
} // namespace rat::cc

#endif
