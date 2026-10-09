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

#include "core.h"
#include "pass/pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;
	struct CallNode;

	struct InlinePass : FunctionPass {
		static constexpr U32 kInlineNodeBudget = 96;			 // max callee size to inline
		static constexpr U32 kMaxInlinesPerFunction = 256; // per-caller fuel
		static constexpr U32 kCallerGrowthBudget = 384;		 // max nodes a caller may gain

		const C8* name() const override;
		void beginModule(Module& module, const CallGraph& graph) override;
		U32 runOnFunction(Function& caller, const TargetInfo& target) override;
		B32 onlyReadsFunction() const override { return false; } // reads callees
	private:
		struct Merged {
			Node* ctrl;
			Node* mem;
			Node* val;
		};

		static B32 isStartProj(const Function& callee, Node* n);
		static B32 isBodyNode(const Function& callee, Node* n);
		static Node* incomingForStartProj(CallNode* call, U32 startProjIdx);
		B32 shouldInline(const Function& caller, CallNode* call, Function* callee);
		void mapNode(Node* key, Node* val);
		Node* mapped(Node* key) const;
		Node* resolve(Node* n) const;
		B32 cloneBody(Function& caller, CallNode* call, Function& callee);
		void wireClones(Function& callee);
		Merged mergeReturns(Function& caller, const Function& callee);
		static void replaceCall(Function& caller, CallNode* call, const Merged& m);
		B32 inlineCallSite(Function& caller, CallNode* call, Function& callee);
	private:
		const CallGraph* graph = nullptr; // of the current module run

		// scratch reused across call sites
		List<Node*> cloneMap; // callee node id -> caller node
		List<CallNode*> worklist;
		List<Node*> ctrls, mems, vals;
	};
} // namespace rat

#endif
