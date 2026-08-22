#include "pass/pass.h"

#include "ir/module.h"

namespace rat {
	Pass::~Pass() = default;
	MachinePass::~MachinePass() = default;

	B32 FunctionPass::run(Module& module, const TargetInfo& target) {
		U32 changed = 0;
		for(Function* fn : module) {
			B32 skippable = onlyReadsFunction();
			if(skippable && fn->isCleanFor(this))
				continue;
			if(runOnFunction(*fn, target))
				++changed;
			else if(skippable)
				fn->markCleanFor(this);
		}
		return changed != 0;
	}
} // namespace rat
