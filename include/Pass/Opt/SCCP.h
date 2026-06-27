// sparse conditional constant propagation: an optimistic data-flow solver that
// jointly discovers constants and which control edges can execute, so a value
// merged at a region (phi) is constant whenever every reachable predecessor
// agrees
//
// references:
// - M. N. Wegman and F. K. Zadeck, "Constant Propagation with Conditional
//   Branches", ACM TOPLAS, 1991
// - C. Click, "Combining Analyses, Combining Optimizations", PhD thesis,
//   Rice University, 1995 (the sea-of-nodes formulation)

#ifndef RAT_PASS_OPT_SCCP_H
#define RAT_PASS_OPT_SCCP_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;
	struct IfNode;
	struct PhiNode;
	struct RegionNode;

	struct SCCPPass : Pass {
		const C8* name() const override;
		B32 run(Module& module) override;
	private:
		U32 runOnFunction(Function& fn);

		struct Lattice {
			enum class Kind : U8 { Top, Constant, Bottom };

			Kind kind = Kind::Top;
			I64 value = 0;

			static Lattice top();
			static Lattice bottom();
			static Lattice constant(I64 v);

			B32 operator==(const Lattice& o) const;
			B32 operator!=(const Lattice& o) const;
		};

		static B32 isValueNode(Node* n);
		static Lattice meet(Lattice a, Lattice b);

		Lattice get(Node* n);
		void markExec(Node* c);
		void pushPhis(RegionNode* r);
		void evalIf(IfNode* iff);
		void visitFlow(Node* c);
		void visitSSA(Node* n);
		Lattice evaluate(Node* n);
		Lattice evalPhi(PhiNode* phi);
		Lattice evalArith(Node* n);
		void solve(Function& fn);
		U32 rewrite(Function& fn);
	private:
		Map<Node*, Lattice> values; // lattice for value producing nodes
		Set<Node*> exec;						// control nodes proven executable
		List<Node*> flowWork;				// executable control nodes to propagate
		List<Node*> ssaWork;				// value / if nodes to re-evaluate
	};
} // namespace rat

#endif
