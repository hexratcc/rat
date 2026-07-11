#ifndef RAT_CODEGEN_REGALLOCBASE_H
#define RAT_CODEGEN_REGALLOCBASE_H

#include "Core.h"

#include "CodeGen/MachineFunction.h"
#include "Support/Pass.h"

namespace rat {
	struct RegAllocBase : MachinePass {
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	protected:
		struct Loc {
			U32 block;
			U32 inst;
		};

		// where a vreg ended up: a physical register, or a frame slot when spilled
		struct Assignment {
			PhysReg reg = kNoReg;
			I32 spillSlot = 0;
			U32 cls = 0;
			B32 spilled = false;
		};

		virtual void resetState() = 0;							 // clear solver state between functions
		virtual void solve() = 0;										 // compute assignments (number() already ran)
		virtual Assignment assignmentOf(VReg v) = 0; // result lookup used by rewrite()

		B32 allocate(MachineFunc& fn,
								 const RegisterInfo& ri,
								 const RegAllocHooks& hooks,
								 List<PhysReg>* usedCalleeSaved);

		void number();
		void pinFixedArgWindows();
		void liveness(List<Set<VReg>>& liveIn, List<Set<VReg>>& liveOut);
		U32 classOf(VReg v) const;
		const RegClass& regClass(U32 cls) const;
		static B32 isCalleeSaved(const RegClass& rc, PhysReg p);
		void rewrite();
		PhysReg scratchAt(U32 cls, U32 idx);
	protected:
		MachineFunc* fn = nullptr;
		const RegisterInfo* ri = nullptr;
		const RegAllocHooks* hooks = nullptr;
		List<Loc> order;								// linear point -> (block, instruction)
		List<List<U32>> blkPts;					// block -> its linear points
		List<I32> callPts;							// points that are calls
		Map<I32, Set<PhysReg>> fixedAt; // physical registers pinned at a point
		Set<PhysReg> usedCallee;
		B32 ok = true;
	};
} // namespace rat

#endif
