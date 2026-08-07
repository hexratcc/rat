#ifndef RAT_SUPPORT_PASSMANAGER_H
#define RAT_SUPPORT_PASSMANAGER_H

#include "codegen/machine_module.h"
#include "core.h"
#include "pass/pass.h"

#include <type_traits>

namespace rat {
	struct Module;
	struct TargetInfo;

	struct PassTiming {
		String name;
		U64 nanos;
		U32 calls;
	};

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
		void gateLastOnChangesSinceSelf();
		void markFixpointEnd() { fixpointEnd = (U32)passes.size(); }
		B32 run(Module& module, std::ostream* log = nullptr);

		void printTimingReport(std::ostream& os) const;
	private:
		void record(const C8* name, U64 nanos);

		const TargetInfo* target;
		List<UniquePtr<Pass>> passes;
		U32 fixpointEnd = 0; // 0 => no fixpoint (single pass over everything)
		List<B32> gated;
		List<UniquePtr<MachinePass>> machinePasses;
		MachineModule mm;
		List<PassTiming> timing;
	};
} // namespace rat

#endif
