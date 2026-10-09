#ifndef RAT_SUPPORT_PASSMANAGER_H
#define RAT_SUPPORT_PASSMANAGER_H

#include "core.h"
#include "pass/pass.h"

#include <iosfwd>

namespace rat {
	struct Module;
	struct TargetInfo;

	struct PassTiming {
		String name;
		U64 nanos;
	};

	namespace detail {
		U64 nowNanos();
		B32 slowerPass(const PassTiming& a, const PassTiming& b);
	} // namespace detail

	struct PassManager {
		explicit PassManager(const TargetInfo& target)
		: target(&target) {}

		template <typename T, typename... Args> T* add(Args&&... args) {
			return static_cast<T*>(add(std::make_unique<T>(std::forward<Args>(args)...)));
		}

		Pass* add(UniquePtr<Pass> pass);
		MachinePass* add(UniquePtr<MachinePass> p);
		void run(Module& module, std::ostream* log = nullptr);

		void printTimingReport(std::ostream& os) const;
	private:
		void record(const C8* name, U64 nanos);
		B32 finish(const C8* name, U64 nanos, B32 changed, std::ostream* log);
		U32 functionPassRun(U32 first) const;
		void runFunctionPasses(U32 first, U32 last, Module& module, std::ostream* log);
		void runMachine(Module& module, std::ostream* log);

		const TargetInfo* target;
		List<UniquePtr<Pass>> passes;
		List<UniquePtr<MachinePass>> machinePasses;
		List<PassTiming> timing;
	};
} // namespace rat

#endif
