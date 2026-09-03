// post-RA peephole: local cleanups lowering and the allocator leave behind.
// Registers and frame slots are value-numbered inside a block, so a copy or a
// spill-slot access whose value is already in place folds away:
//
//   mov [rbp-8],rax        mov [rbp-8],rax
//   mov rcx,[rbp-8]   ->   mov rcx,rax      (slot value still sits in a reg)
//   mov rax,rcx            <deleted>        (both sides already equal)
//
// A second phase runs a backward demanded-bits dataflow over the physical
// registers and deletes the width normalizations (SignExtBits/MaskBits) whose
// high bits no reader observes. A third phase runs a backward liveness
// dataflow over the spill slots and deletes the stores no reload, call 
// argument, or overwrite ever observes.

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
		static constexpr U64 kAllBits = ~(U64)0;

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

		// demanded-bits phase
		static U64 lowMask(U32 n);
		static U64 carryMask(U64 out);
		static B32 isNormalize(X86Op op);
		static void
		demandUses(const MachineInstr& in, U64 mask, U64* dem, U32 from = 0, U32 to = ~(U32)0);
		static B32 immCount(const MachineInstr& in, U32& out);
		static B32 readsFlags(X86Op op);
		static B32 writesFlags(X86Op op);
		static B32 flagSafeToDrop(const MachineBlock& b, U32 at);
		static void transfer(const MachineInstr& in, U64* dem);
		static void eraseMarked(MachineBlock& b, const List<B32>& drop);
		U32 elimRedundantExt(MachineFunc& mf);

		// dead-slot-store phase
		static B32 isAnySlotStore(const MachineInstr& in);
		static B32 isAnySlotLoad(const MachineInstr& in);
		static void slotBlockOut(const MachineBlock& b, const List<List<U64>>& liveIn, List<U64>& cur);
		static void slotStep(const MachineInstr& in,
												 const Map<I32, U32>& slotIdx,
												 const Map<I32, U32>& readWidth,
												 List<U64>& cur);
		U32 elimDeadSlotStores(MachineFunc& mf);
	private:
		ValueState st;
	};
} // namespace rat

#endif
