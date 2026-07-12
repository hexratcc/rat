#include "Support/Pass.h"

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
} // namespace rat
