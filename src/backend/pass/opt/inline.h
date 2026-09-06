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
		// call graph row
		struct Info {
			Function* fn = nullptr;
			List<Info*> callees;	// direct callees, sorted, live functions only
			U64 version = kNoRow; // caller version the row was built from
			U64 quietAt = 0;			// stamp of the last run that inlined nothing, 0 = none
			U32 firstSize = 0;		// caller size when first seen, bounds growth per module run
			U32 visit = 0;				// dfs stamp
			I8 cyclic = -1;				// cached isCyclic, -1 = unknown
		};
		static constexpr U64 kNoRow = ~(U64)0;

		void syncCallGraph(Module& m);
		void dropDeadRows(Module& m);
		B32 refreshCallees(Function& fn, Info& info);
		void forgetCycles();
		Function* lookup(const String& name) const;
		B32 reaches(Info* from, Info* target);
		B32 isCyclic(Function* fn);
		U64 quietStamp(const Function& caller, const Info& info) const;

		B32 isStartProj(const Function& callee, Node* n);
		Node* incomingForStartProj(CallNode* call, U32 startProjIdx);
		B32 shouldInline(const Function& caller, CallNode* call, Function* callee);
		B32
		inlineCallSite(Function& caller, CallNode* call, Function& callee, List<CallNode*>& newCalls);

		Module* module = nullptr;
		Map<const Function*, Info> infos;
		Map<String, Function*> byName;
		U32 visitCur = 0;

		// scratch reused across call sites
		List<Node*> cloneMap; // callee node id -> caller node
		List<CallNode*> worklist;
		List<Node*> ctrls, mems, vals;
	};
} // namespace rat

#endif
