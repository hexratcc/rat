// linear-scan register allocation over the target-independent machine IR. virtual registers are
// numbered along a linearized instruction order, live intervals are computed, and physical
// registers are assigned per register class; under pressure a value is spilled to a frame
// slot. the allocator is fully backend-agnostic: it reads register classes from a
// RegisterInfo and builds spill/reload/slot constructs through RegAllocHooks callbacks, so
// it never names a single target opcode
//
// references:
// - M. Poletto and V. Sarkar, "Linear Scan Register Allocation", ACM TOPLAS, 1999
// - P. Briggs, K. D. Cooper, T. J. Harvey, L. T. Simpson, "Practical Improvements to the
//   Construction and Destruction of Static Single Assignment Form", SP&E, 1998

#ifndef RAT_CODEGEN_REGALLOC_H
#define RAT_CODEGEN_REGALLOC_H

#include "Core.h"

#include "CodeGen/MachineFunction.h"
#include "Pass/Pass.h"

namespace rat {
	struct RegAllocHooks {
		MachineInstr (*makeReload)(PhysReg dst, I32 slot, U32 cls, U32 width) = nullptr;
		MachineInstr (*makeSpill)(I32 slot, PhysReg src, U32 cls, U32 width) = nullptr;
		I32 (*allocSlot)(MachineFunc& fn, U32 cls, U32 width) = nullptr;
	};

	B32 allocateRegisters(MachineFunc& fn,
												const RegisterInfo& ri,
												const RegAllocHooks& hooks,
												List<PhysReg>* usedCalleeSaved = nullptr);

	struct RegAllocPass : MachinePass {
		const C8* name() const override { return "regalloc"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		U32 runOnMachineFunction(const Function& fn, MachineFunc& mf, const TargetInfo& target);
	};
} // namespace rat

#endif
