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
		B32 run(Module& module, const TargetInfo& target) override;
		U32 runOnFunction(Function& caller, const TargetInfo& target) override;
		B32 onlyReadsFunction() const override { return false; } // reads callees
	private:
		B32 isStartProj(const Function& callee, Node* n);
		void buildCallGraph(Module& m);
		void refreshCallees(Function& fn);
		Function* lookup(const String& name) const;
		B32 reachesIndex(U32 from, U32 target);
		B32 isCyclic(Function* fn);
	private:
		Module* graphModule = nullptr;
		Map<const Function*, U64> quietAt;
		List<Function*> graphFuncs;
		List<List<U32>> graphCallees;
		Map<String, U32> graphByName;
		Map<const Function*, U32> graphIndex;
		List<U32> visitStamp;
		List<U32> edgeStamp;
		U32 visitStampCur = 0;
		U32 edgeStampCur = 0;
		Map<const Function*, B32> cyclicCache;

		Node* incomingForStartProj(CallNode* call, U32 startProjIdx);
		B32 shouldInline(const Function& caller, CallNode* call, Function* callee);

		B32
		inlineCallSite(Function& caller, CallNode* call, Function& callee, List<CallNode*>& newCalls);

		// callee node id -> caller node, reused across call sites
		List<Node*> cloneMap;
		List<CallNode*> worklist;
		List<Node*> ctrls, mems, vals;
	};
} // namespace rat

#endif
