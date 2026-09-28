#ifndef RAT_SUPPORT_PASSMANAGER_H
#define RAT_SUPPORT_PASSMANAGER_H

#include "codegen/machine_module.h"
#include "core.h"
#include "pass/pass.h"

#include <iosfwd>

namespace rat {
	struct Module;
	struct TargetInfo;

	struct PassTiming {
		String name;
		U64 nanos;
		U32 calls;
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
		void gateLastOnChangesSinceSelf();
		void markFixpointEnd() { fixpointEnd = (U32)passes.size(); }
		void run(Module& module, std::ostream* log = nullptr);

		void printTimingReport(std::ostream& os) const;
	private:
		void record(const C8* name, U64 nanos);
		B32 finish(const C8* name, U64 start, B32 changed, std::ostream* log);
		B32 isDue(U32 i, const List<B32>& changedAt) const;
		void runAt(U32 i, Module& module, List<B32>& changedAt, std::ostream* log);

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
