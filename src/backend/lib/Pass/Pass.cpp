#include "Pass/Pass.h"

#include "IR/Module.h"

namespace rat {
	Pass::~Pass() = default;
	MachinePass::~MachinePass() = default;

	B32 FunctionPass::run(Module& module, const TargetInfo& target) {
		U32 changed = 0;
		for(Function* fn : module)
			changed += runOnFunction(*fn, target);
		return changed != 0;
	}
} // namespace rat
