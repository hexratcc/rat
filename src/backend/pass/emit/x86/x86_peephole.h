// post-RA peephole: local cleanups lowering and the allocator leave behind.
// Registers and frame slots are value-numbered inside a block, so a copy or a
// spill-slot access whose value is already in place folds away:
//
//   mov [rbp-8],rax        mov [rbp-8],rax
//   mov rcx,[rbp-8]   ->   mov rcx,rax      (slot value still sits in a reg)
//   mov rax,rcx            <deleted>        (both sides already equal)
//
// A second phase runs a backward liveness dataflow over the spill slots and
// deletes the stores no reload, call argument, or overwrite ever observes.

#ifndef RAT_PASS_EMIT_X86PEEPHOLE_H
#define RAT_PASS_EMIT_X86PEEPHOLE_H

#include "core.h"

#include "pass/emit/x86/x86_op.h"
#include "pass/pass.h"

namespace rat {
	namespace detail {
		void eraseMarked(MachineFunc& mf, MachineBlock& b, const List<B32>& drop, B32 kill);
	} // namespace detail

	struct X86PeepholePass : MachinePass {
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
	private:
		static constexpr U32 kMaxPhys = X86Target::kRegEnd;
		static constexpr U64 kAllBits = ~(U64)0;
		static constexpr U32 kNoBit = ~0u;

		enum class Fold {
			None,
			Rewritten,
			Dropped
		};

		// an 8-byte spill slot, the last one is shared by every other frame offset, see slotKey.
		// value and width belong to the value numbering, the rest to the dead-slot-store phase
		struct Slot {
			U32 value = 0; // value number, stale below validFrom
			U32 width = 0; // of value
			U32 readWidth = 0;
			U32 bit = kNoBit; // liveness bit while only spill stores, reloads and call arguments touch it
			B32 seen = false;
			B32 untracked = false;
		};
	private:
		U32 slotKey(I32 s) const;
		static B32 isAnySlotLoad(const MachineInstr& in);

		// value-numbering phase
		U32 regValue(PhysReg p) const;
		void setReg(PhysReg p, U32 v);
		U32 slotValue(I32 s, U32 width) const;
		void setSlot(I32 s, U32 width, U32 v);
		void killAllRegs();
		void forgetAll();
		Fold foldCopy(const MachineInstr& in);
		Fold foldSpill(const MachineInstr& in);
		Fold foldReload(MachineInstr& in);
		Fold fold(MachineInstr& in);
		U32 foldBlock(MachineFunc& mf, MachineBlock& b, List<B32>& drop);

		// dead-slot-store phase
		void solve(const MachineFunc& mf, List<U64>& liveIn, List<U64>& cur);
		static void blockOut(const MachineBlock& b, const List<U64>& liveIn, List<U64>& cur);
		B32 noteSlots(const MachineInstr& in);
		void slotStep(const MachineInstr& in, List<U64>& cur) const;
		U32 elimDeadSlotStores(MachineFunc& mf);
		U32 markDeadStores(const MachineBlock& b, List<U64>& cur, List<B32>& drop);
	private:
		U32 reg[kMaxPhys]; // value number of each register, 0 unknown
		List<Slot> slots;	 // by slotKey
		List<B32> touches; // block -> has a slot store, reload or call slot argument
		U32 nextValue = 1;
		U32 validFrom = 1; // slot values below are from before the last reset
	};
} // namespace rat

#endif
