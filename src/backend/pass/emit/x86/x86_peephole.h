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
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
	private:
		static constexpr U32 kMaxPhys = X86Target::kStBase + 8; // every x86 register
		static constexpr U32 kDemRegs = X86Target::kXmmBase;
		static constexpr U64 kAllBits = ~(U64)0;
	private:
		static U32 slotKey(I32 s, U32 keys);

		struct SlotValue {
			U32 value = 0;
			U32 width = 0;
			U32 epoch = 0; // stale unless it matches the state epoch
		};

		struct ValueState {
			U32 reg[kMaxPhys];
			List<SlotValue> slot; // by slotKey
			U32 keys = 0;
			U32 epoch = 1;
			U32 next = 1;

			U32 fresh() { return next++; }
			void begin(U32 frameKeys);
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
		U32 foldRegCopy(MachineInstr& in, B32& keep);
		U32 foldSlotStore(MachineInstr& in, B32& keep);
		U32 foldSlotLoad(MachineInstr& in, B32& keep);
		void stepOther(const MachineInstr& in);
		U32 runOnBlock(MachineBlock& b);

		// demanded-bits phase
		static B32 tracked(const MachineOperand& o) { return o.isPhys() && o.phys < kDemRegs; }
		static U64 lowMask(U32 n);
		static U64 carryMask(U64 out);
		static B32 isNormalize(X86Op op);
		static B32 hasNormalize(const MachineBlock& b);
		static void demandUses(const MachineInstr& in, U64 mask, U64* dem, U32 from = 0, U32 to = ~0u);
		static B32 immCount(const MachineInstr& in, U32& out);
		static B32 readsFlags(X86Op op);
		static B32 writesFlags(X86Op op);
		static B32 flagSafeToDrop(const MachineBlock& b, U32 at);
		static void transfer(const MachineInstr& in, U64* dem);
		static void eraseMarked(MachineBlock& b, const List<B32>& drop);
		static B32 orInto(U64* into, const List<U64>& from);
		void seedWork(const MachineFunc& mf, List<U32>& work, List<B32>& queued);
		void postorder(const MachineFunc& mf);
		static void queuePreds(const MachineBlock& b, List<U32>& work, List<B32>& queued);
		U32 elimRedundantExt(MachineFunc& mf);
	private:
		// dead-slot-store phase
		static constexpr U32 kNoBit = ~0u;

		// dense
		struct TrackedSlots {
			List<U32> index;
			List<U32> readWidth;
			List<B32> touches; // block id -> has a slot store, reload or call slot argument
			List<U8> seen;
			List<U8> untracked;
			U32 keys = 0;
			U32 count = 0;

			U32 key(I32 s) const;
			void begin(const MachineFunc& mf);
			void note(const MachineInstr& in, U32 block);
			void finish();
		};

		static B32 isAnySlotStore(const MachineInstr& in);
		static B32 isAnySlotLoad(const MachineInstr& in);
		static void slotBlockOut(const MachineBlock& b, const List<U64>& liveIn, List<U64>& cur);
		static void slotStep(const MachineInstr& in, const TrackedSlots& slots, List<U64>& cur);
		U32 elimDeadSlotStores(MachineFunc& mf);

		ValueState st;
		TrackedSlots slots;
		List<U32> seed;				// block postorder
		List<B32> seedQueued; // block -> in seed, empty until computed
		List<B32> drop;				// per instruction of the block being swept
	};
} // namespace rat

#endif
