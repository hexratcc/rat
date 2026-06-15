#include "Pass/PassManager.h"

#include "IR/Module.h"

namespace rat {
	B32 PassManager::run(Module& module) {
		B32 changed = false;
		for (auto& pass : passes)
			if (pass->run(module))
				changed = true;
		return changed;
	}
} // namespace rat
