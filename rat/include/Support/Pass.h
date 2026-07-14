#ifndef RAT_SUPPORT_PASS_H
#define RAT_SUPPORT_PASS_H

#include "Core.h"

namespace rat {
	struct Function;
	struct MachineFunc;
	struct MachineModule;
	struct Module;
	struct TargetInfo;

	struct Pass {
		virtual ~Pass();

		virtual const C8* name() const = 0;
		virtual B32 run(Module& module) = 0;
	};

	struct FunctionPass : Pass {
		B32 run(Module& module) override;

		virtual U32 runOnFunction(Function& fn) = 0;
	};

	// post-lowering pass over machine state, the pass manager runs all IR passes first, then machine
	// passes in order
	struct MachinePass {
		virtual ~MachinePass();

		virtual const C8* name() const = 0;
		virtual B32 run(Module& module, MachineModule& mm, const TargetInfo& target) = 0;
	};
} // namespace rat

#endif
