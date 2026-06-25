#include "Pass/PassManager.h"

#include "IR/Module.h"

namespace rat {
	Pass* PassManager::add(UniquePtr<Pass> pass) {
		Pass* raw = pass.get();
		passes.push_back(std::move(pass));
		return raw;
	}

	B32 PassManager::run(Module& module, std::ostream* log) {
		B32 changed = false;
		for(auto& pass : passes) {
			B32 c = pass->run(module);
			if(log)
				*log << "; " << pass->name() << (c ? " : changed\n" : " : unchanged\n");
			changed = changed || c;
		}
		return changed;
	}
} // namespace rat
