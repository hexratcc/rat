// loop induction-variable strength reduction

#ifndef RAT_PASS_OPT_STRENGTHREDUCE_H
#define RAT_PASS_OPT_STRENGTHREDUCE_H

#include "core.h"

#include "pass/pass.h"

namespace rat {
	struct ConstantNode;
	struct Node;
	struct PhiNode;

	namespace detail {
		B32 matchLinearIV(PhiNode* p, I64& step, U32& recIdx);
		ConstantNode* affineScale(Node* u, PhiNode* p);
	} // namespace detail

	struct StrengthReducePass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	};
} // namespace rat

#endif
