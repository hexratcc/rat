// graph-coloring register allocation over the machine IR
//
// references:
// - G. J. Chaitin, "Register Allocation and Spilling via Graph Coloring", SIGPLAN, 1982
// - P. Briggs, K. D. Cooper, K. Kennedy, L. Torczon, "Coloring Heuristics for Register
//   Allocation", PLDI, 1989

#ifndef RAT_CODEGEN_GRAPHREGALLOC_H
#define RAT_CODEGEN_GRAPHREGALLOC_H

#include "Core.h"

#include "CodeGen/RegAllocBase.h"

namespace rat {
	struct GraphColorRegAllocPass : RegAllocBase {
		static constexpr U32 kAdjCap = 128;

		const C8* name() const override { return "graph-regalloc"; }
	private:
		// one interference graph node per virtual register
		struct Node {
			VReg vreg = kNoVReg;
			U32 cls = 0;
			I32 start = -1; // first point the value is live
			I32 end = -1;		// last point the value is live
			U32 uses = 0;		// def/use count
			U32 degree = 0; // true interference degree
			PhysReg color = kNoReg;
			I32 spillSlot = 0;
			B32 spilled = false;
			B32 crossesCall = false;
			B32 mustSpill = false;
			Set<VReg> adj;					// interference neighbors
			Set<PhysReg> forbidden; // precolored constraints
		};

		void resetState() override {
			nodes.clear();
			selectStack.clear();
		}
		void solve() override;
		Assignment assignmentOf(VReg v) override;

		void buildInterference();
		Node& nodeFor(VReg v);
		void addHalfEdge(Node& n, VReg other);
		void addEdge(VReg a, VReg b);
		void interfereAll(const Set<VReg>& live);
		void computeForbidden();

		void simplify();
		U32 colorCount(U32 cls) const;
		F64 spillCost(const Node& n) const;
		void selectColors();
	private:
		Map<VReg, Node> nodes;
		List<VReg> selectStack; // simplify order; popped in reverse during select
	};
} // namespace rat

#endif
