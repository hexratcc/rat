// function inlining: replace a call to a small, non-recursive function with a
// clone of the callee's body, splicing the callee's control and memory edges
// into the caller and merging its returns at the call's continuation
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995
// - S. Muchnick, "Advanced Compiler Design and Implementation", 1997, ch. 15

#ifndef RAT_PASS_OPT_INLINE_H
#define RAT_PASS_OPT_INLINE_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;
	struct CallNode;

	struct InlinePass : Pass {
		static constexpr U32 kInlineNodeBudget = 64;			 // max callee size to inline
		static constexpr U32 kMaxInlinesPerFunction = 256; // per-caller fuel

		const C8* name() const override;
		B32 run(Module& module) override;
	private:
		B32 isStartProj(const Function& callee, Node* n);

		Node* incomingForStartProj(CallNode* call, U32 startProjIdx);
		B32 shouldInline(const Function& caller, CallNode* call, Function* callee);

		B32 inlineCallSite(Function& caller, CallNode* call, Function& callee);
		U32 inlineInto(Function& caller, Module& m);
	};
} // namespace rat

#endif
