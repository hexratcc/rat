#ifndef RAT_CODEGEN_REGALLOC_H
#define RAT_CODEGEN_REGALLOC_H

#include "core.h"

#include "codegen/machine_function.h"
#include "pass/pass.h"

namespace rat {
	namespace detail {
		PhysReg firstFree(const List<PhysReg>& regs, U64 blocked);

		using RaSeg = Pair<I32, I32>;

		struct RaInterval {
			List<RaSeg> segs;
			F32 weight = 0;
			PhysReg reg = kNoReg;
			I32 slot = 0;
		};

		struct RegAllocFunc {
			RegAllocFunc(MachineFunc& f, const RegisterInfo& r, const RegAllocHooks& h);
			void run();
		private:
			// intervals
			void number();
			void pinFixed(U32 b);
			void liveness();
			void buildIntervals();
			void addSeg(VReg v, I32 start, I32 end);
			// assignment
			void assignRegs();
			PhysReg pick(VReg v) const;
			// rewrite
			void rewrite();
			void rewriteInstr(List<MachineInstr>& out, MachineInstr& in, U32 i);
			PhysReg spillReg(List<MachineInstr>& out, const MachineOperand& o, U32 i, B32 use);
			PhysReg pickTemp(U32 cls, U64 hard, U64 soft);
			// queries
			B32 isCopy(const MachineInstr& in) const;
		private:
			MachineFunc& fn;
			const RegisterInfo& ri;
			const RegAllocHooks& hooks;
			U32 nv;
			U64 allocMask[kMaxRegClasses] = {};
			U64 calleeMask = 0;
			// numbering
			List<U32> blockFirst; // block -> first instruction, one past the end at the back
			List<U64> busy;				// slot -> busy physical registers
			U64 usedCallee = 0;
			// liveness
			List<List<VReg>> liveOut; // block -> live-out vregs
			List<RaInterval> iv;
			// rewrite of the current instruction
			List<Pair<VReg, PhysReg>> temps; // spilled vreg -> its temp
			U64 taken = 0;									 // temps
			U64 own = 0;										 // clobbers
			List<MachineInstr> stores;
		};
	} // namespace detail

	struct RegAllocPass : MachinePass {
		const C8* name() const override { return "regalloc"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	};
} // namespace rat

#endif
