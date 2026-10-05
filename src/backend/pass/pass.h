#ifndef RAT_SUPPORT_PASS_H
#define RAT_SUPPORT_PASS_H

#include "core.h"

namespace rat {
	struct Function;
	struct MachineFunc;
	struct Module;
	struct TargetInfo;

	struct Pass {
		virtual ~Pass();

		virtual const C8* name() const = 0;
		virtual B32 run(Module& module, const TargetInfo& target) = 0;
	};

	struct FunctionPass : Pass {
		B32 run(Module& module, const TargetInfo& target) override;

		virtual U32 runOnFunction(Function& fn, const TargetInfo& target) = 0;
		virtual B32 onlyReadsFunction() const { return true; }
	};

	// post-lowering pass over machine state, the pass manager runs all IR passes first, then every
	// machine pass in order on one function at a time
	struct MachinePass {
		virtual ~MachinePass();

		virtual const C8* name() const = 0;
		virtual B32
		run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) = 0;
		virtual void finish(Module& module, const TargetInfo& target); // after the last function
	};
} // namespace rat

#endif
