#ifndef RAT_SUPPORT_PASSMANAGER_H
#define RAT_SUPPORT_PASSMANAGER_H

#include "CodeGen/MachineModule.h"
#include "Core.h"
#include "Support/Pass.h"

#include <type_traits>

namespace rat {
	struct Module;
	struct TargetInfo;

	struct PassManager {
		explicit PassManager(const TargetInfo& target)
		: target(&target) {}

		template <typename T, typename... Args> T* add(Args&&... args) {
			auto owned = std::make_unique<T>(std::forward<Args>(args)...);
			T* raw = owned.get();
			if constexpr(std::is_base_of_v<MachinePass, T>)
				machinePasses.push_back(std::move(owned));
			else
				passes.push_back(std::move(owned));
			return raw;
		}

		Pass* add(UniquePtr<Pass> pass);
		B32 run(Module& module, std::ostream* log = nullptr);
	private:
		const TargetInfo* target;
		List<UniquePtr<Pass>> passes;
		List<UniquePtr<MachinePass>> machinePasses;
		MachineModule mm;
	};
} // namespace rat

#endif
