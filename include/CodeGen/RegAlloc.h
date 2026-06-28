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
#include "Support/Pass.h"

namespace rat {
	struct RegAllocHooks {
		Delegate<MachineInstr(PhysReg dst, I32 slot, U32 cls, U32 width)> makeReload;
		Delegate<MachineInstr(I32 slot, PhysReg src, U32 cls, U32 width)> makeSpill;
		Delegate<I32(MachineFunc& fn, U32 cls, U32 width)> allocSlot;
	};

	struct LinearRegAllocPass : MachinePass {
		const C8* name() const override { return "linear-regalloc"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		struct Interval {
			VReg vreg = kNoVReg;
			U32 cls = 0;
			I32 start = -1;
			I32 end = -1;
			PhysReg assigned = kNoReg;
			I32 spillSlot = 0;
			B32 spilled = false;
			B32 crossesCall = false;
		};

		struct Loc {
			U32 block;
			U32 inst;
		};

		B32 allocate(MachineFunc& fn,
								 const RegisterInfo& ri,
								 const RegAllocHooks& hooks,
								 List<PhysReg>* usedCalleeSaved);

		void number();
		void pinFixedArgWindows();
		void liveness(List<Set<VReg>>& liveIn, List<Set<VReg>>& liveOut);
		void extend(VReg v, U32 cls, I32 point);
		U32 classOf(VReg v) const;
		void buildIntervals();
		const RegClass& regClass(U32 cls) const;
		Set<PhysReg> forbidden(const Interval& iv) const;
		void assignRegs();
		static B32 isCalleeSaved(const RegClass& rc, PhysReg p);
		void spillAt(Interval* cur, List<Interval*>& active);
		void rewrite();
		PhysReg scratchAt(U32 cls, U32 idx);
	private:
		MachineFunc* fn = nullptr;
		const RegisterInfo* ri = nullptr;
		const RegAllocHooks* hooks = nullptr;
		List<Loc> order;
		List<List<U32>> blkPts;
		List<I32> callPts;
		Map<I32, Set<PhysReg>> fixedAt;
		Map<VReg, Interval> intervals;
		Set<PhysReg> usedCallee;
		B32 ok = true;
	};
} // namespace rat

#endif
