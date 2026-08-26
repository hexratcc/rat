// post-RA peephole: local cleanups lowering and the allocator leave behind.
// Registers and frame slots are value-numbered inside a block, so a copy or a
// spill-slot access whose value is already in place folds away:
//
//   mov [rbp-8],rax        mov [rbp-8],rax
//   mov rcx,[rbp-8]   ->   mov rcx,rax      (slot value still sits in a reg)
//   mov rax,rcx            <deleted>        (both sides already equal)

#ifndef RAT_PASS_EMIT_X86PEEPHOLE_H
#define RAT_PASS_EMIT_X86PEEPHOLE_H

#include "core.h"

#include "pass/emit/x86/x86_op.h"
#include "pass/pass.h"

namespace rat {
	struct X86PeepholePass : MachinePass {
		const C8* name() const override { return "x86-peephole"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		static constexpr U32 kMaxPhys = 64;

		struct ValueState {
			U32 reg[kMaxPhys];
			Map<I32, U32> slot;
			Map<I32, U32> slotWidth;
			U32 next = 1;

			U32 fresh() { return next++; }
			void reset();
			void killAllRegs();
			void killReg(PhysReg p);
			U32 valueOf(PhysReg p) const { return p < kMaxPhys ? reg[p] : 0; }
			void setReg(PhysReg p, U32 v);
			PhysReg regHolding(U32 v, PhysReg except) const;
			U32 slotValue(I32 s, U32 width) const;
			void setSlot(I32 s, U32 width, U32 v);
		};

		// value-numbering phase
		static B32 isTransparent(X86Op op);
		static B32 isRegCopy(const MachineInstr& in);
		static B32 isSlotStore(const MachineInstr& in);
		static B32 isSlotLoad(const MachineInstr& in);
		static MachineInstr makeCopy(PhysReg dst, PhysReg src, U32 cls, U32 width);
		static B32 writesUntrackedSlot(const MachineInstr& in);
		U32 runOnBlock(MachineBlock& b);
	private:
		ValueState st;
	};
} // namespace rat

#endif
