#ifndef RAT_PASS_OPT_FOLD_H
#define RAT_PASS_OPT_FOLD_H

#include "Core.h"
#include "IR/Opcode.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Node;
	struct Type;

	Node* constant(Function& fn, Type* type, I64 value);

	Node* foldBinary(Function& fn, Opcode op, Node* lhs, Node* rhs);
	Node* foldUnary(Function& fn, Opcode op, Node* operand);
	Node* foldCompare(Function& fn, Opcode op, Node* lhs, Node* rhs);
	Node* foldConvert(Function& fn, Opcode op, Node* operand, Type* destType);

	Node* simplify(Function& fn, Node* n);

	struct FoldPass : Pass {
		const char* name() const override;
		B32 run(Module& module) override;
	};
} // namespace rat

#endif
