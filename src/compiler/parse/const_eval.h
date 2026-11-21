#ifndef RAT_CC_CONST_EVAL_H
#define RAT_CC_CONST_EVAL_H

#include "parse/ast.h"
#include "target_layout.h"

namespace rat::cc {
	namespace detail {
		B32 floatToInt(F80 v, CType to, I64& out);
	} // namespace detail

	// folds a constant over the types the parser gave, false when it is not one
	// every value is wrapped or rounded to the type of its expression
	struct ConstEval {
		explicit ConstEval(const TargetLayout& layout)
		: lay(layout) {}
		virtual ~ConstEval() = default;

		B32 evalInt(const Expr* e, I64& out) const;
		B32 evalFloat(const Expr* e, F80& out) const;
		B32 evalAs(const Expr* e, CType to, I64& out) const;
		// sizeof or _Alignof from the type, the emitter knows more, see Emitter::ConstFolder
		virtual B32 sizeOf(const Expr* e, U64& out) const;

		const TargetLayout& lay;
	private:
		B32 evalIntNode(const Expr* e, I64& out) const;
		B32 evalFloatNode(const Expr* e, F80& out) const;
		B32 evalTruth(const Expr* e, B32& out) const;
		B32 evalUnary(const Expr* e, I64& out) const;
		B32 evalBinary(const Expr* e, I64& out) const;
		B32 evalLogical(const Expr* e, I64& out) const;
		B32 evalPointerOp(ExprOp op, I64 a, CType at, I64 b, CType bt, I64& out) const;
		B32 evalShift(ExprOp op, I64 a, I64 b, CType rt, I64& out) const;
		B32 evalArith(ExprOp op, I64 a, I64 b, B32 isUnsigned, I64& out) const;
		I64 wrapTo(I64 v, CType t) const;
	};

	I64 typeClassOf(CType t);
	CType sizeType(const TargetLayout& lay);
	CType ptrDiffType(const TargetLayout& lay);
} // namespace rat::cc

#endif
