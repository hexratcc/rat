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

#ifndef RAT_CODEGEN_LINEARSCANREGALLOC_H
#define RAT_CODEGEN_LINEARSCANREGALLOC_H

#include "Core.h"

#include "CodeGen/RegAllocBase.h"

namespace rat {
	struct LinearScanRegAllocPass : RegAllocBase {
		const C8* name() const override { return "linear-scan-regalloc"; }
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

		void resetState() override { intervals.clear(); }
		void solve() override;
		Assignment assignmentOf(VReg v) override;

		void extend(VReg v, U32 cls, I32 point);
		void buildIntervals();
		Set<PhysReg> forbidden(const Interval& iv) const;
		void assignRegs();
		void spillAt(Interval* cur, List<Interval*>& active);
	private:
		Map<VReg, Interval> intervals;
	};
} // namespace rat

#endif
