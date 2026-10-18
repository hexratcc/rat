// loop induction-variable strength reduction

#ifndef RAT_PASS_OPT_STRENGTHREDUCE_H
#define RAT_PASS_OPT_STRENGTHREDUCE_H

#include "core.h"

#include "ir/opcode.h"
#include "pass/pass.h"

namespace rat {
	struct CompareNode;
	struct ConstantNode;
	struct Node;
	struct PhiNode;

	namespace detail {
		B32 matchLinearIV(PhiNode* p, I64& step, U32& recIdx);
		B32 foldableScale(Opcode op, I64 k, U32 width);
		ConstantNode* affineScale(Node* u, PhiNode* p);
		CompareNode* loopTest(PhiNode* p, U32 recIdx);
		ConstantNode* offsetOf(Node* u, PhiNode* p);
		B32 isSext64(Node* u);
		B32 widenableUse(Node* u, PhiNode* p, I64 lo, I64 top);
		void widenUse(Function& fn, Node* u, PhiNode* p, PhiNode* q, CompareNode* cmp, I64 bound);
	} // namespace detail

	struct StrengthReducePass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
		static U32 widen(Function& fn, PhiNode* p, I64 step, U32 recIdx);
	};
} // namespace rat

#endif
