#ifndef RAT_PASS_PASSMANAGER_H
#define RAT_PASS_PASSMANAGER_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Module;

	struct PassManager {
		template <typename T, typename... Args> T* add(Args&&... args) {
			auto owned = std::make_unique<T>(std::forward<Args>(args)...);
			T* raw = owned.get();
			passes.push_back(std::move(owned));
			return raw;
		}

		B32 run(Module& module);

	private:
		List<UniquePtr<Pass>> passes;
	};
} // namespace rat

#endif
