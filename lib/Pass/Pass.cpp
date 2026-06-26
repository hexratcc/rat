#include "Pass/Pass.h"

#include "CodeGen/MachineModule.h"
#include "IR/Module.h"

namespace rat {
	Pass::~Pass() = default;
	MachinePass::~MachinePass() = default;

	B32 FunctionPass::run(Module& module) {
		U32 changed = 0;
		for(Function* fn : module)
			changed += runOnFunction(*fn);
		return changed != 0;
	}

	B32 MachineFunctionPass::run(Module& module, MachineModule& mm, const TargetInfo& target) {
		U32 changed = 0;
		for(Function* fn : module)
			changed += runOnMachineFunction(*fn, mm.get(fn), target);
		return changed != 0;
	}
} // namespace rat
