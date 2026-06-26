#ifndef RAT_PASS_PASS_H
#define RAT_PASS_PASS_H

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
		B32 run(Module& module) final;
		virtual U32 runOnFunction(Function& fn) = 0;
	};

	struct MachinePass {
		virtual ~MachinePass();

		virtual const C8* name() const = 0;
		virtual B32 run(Module& module, MachineModule& mm, const TargetInfo& target) = 0;
	};

	struct MachineFunctionPass : MachinePass {
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) final;
		virtual U32
		runOnMachineFunction(const Function& fn, MachineFunc& mf, const TargetInfo& target) = 0;
	};
} // namespace rat

#endif
