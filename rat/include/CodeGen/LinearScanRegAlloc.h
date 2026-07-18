// linear-scan register allocation over the target-independent machine IR. virtual registers
// are numbered along a linearized instruction order and live ranges are computed as segment
// lists (hole-aware: the gaps between segments are provably off every def-use path, so
// fixed-register pins inside a hole don't constrain the value and call clobbers inside a
// hole don't force a callee-saved register). assignment scans ranges in start order per
// register class; copy hints bias the choice so coalescable moves become elided self-moves,
// and move partners whose ranges meet only at their connecting copies may share a register.
// under pressure a value is spilled to a frame slot. the allocator is fully
// backend-agnostic: it reads register classes from a RegisterInfo and builds
// spill/reload/slot constructs through RegAllocHooks callbacks, so it never names a single
// target opcode
//
// references:
// - M. Poletto and V. Sarkar, "Linear Scan Register Allocation", ACM TOPLAS, 1999
// - O. Traub, G. Holloway, M. D. Smith, "Quality and Speed in Linear-scan Register
//   Allocation", PLDI, 1998

#ifndef RAT_CODEGEN_LINEARSCANREGALLOC_H
#define RAT_CODEGEN_LINEARSCANREGALLOC_H

#include "Core.h"

#include "CodeGen/RegAllocBase.h"

namespace rat {
	struct LinearScanRegAllocPass : RegAllocBase {
		const C8* name() const override { return "linear-scan-regalloc"; }
	private:
		// closed [start, end]
		struct Seg {
			I32 start;
			I32 end;
		};

		struct Interval {
			VReg vreg = kNoVReg;
			U32 cls = 0;
			I32 start = -1; // hull: segs.front().start
			I32 end = -1;		// hull: segs.back().end
			List<Seg> segs;
			PhysReg assigned = kNoReg;
			I32 spillSlot = 0;
			B32 spilled = false;
			B32 crossesCall = false;
		};

		void resetState() override {
			intervals.clear();
			pinsByPoint.clear();
		}
		void solve() override;
		Assignment assignmentOf(VReg v) override;
		void buildIntervals();
		static void coalesceSegs(Interval& iv);
		static B32 coversPoint(const Interval& iv, I32 pt);
		static B32 overlapOnlyAt(const Interval& a, const Interval& b, const List<I32>& pts);
		Set<PhysReg> forbidden(const Interval& iv) const;
		void assignRegs();
		void assignSpillSlots();
		void spillAt(Interval* cur, List<Interval*>& active);
	private:
		Map<VReg, Interval> intervals;
		List<std::pair<I32, const Set<PhysReg>*>> pinsByPoint;
	};
} // namespace rat

#endif
