#ifndef RAT_CC_AST_H
#define RAT_CC_AST_H

#include "Core.h"

namespace rat::cc {
	enum class ExprOp : U8 {
		// unary
		Pos,    // +x
		Neg,    // -x
		Not,    // !x
		BitNot, // ~x

		// binary arithmetic
		Add,
		Sub,
		Mul,
		Div,
		Rem,

		// shifts
		Shl,
		Shr,

		// relational
		Lt,
		Gt,
		Le,
		Ge,
		Eq,
		Ne,

		// bitwise
		BitAnd,
		BitOr,
		BitXor,

		// logical
		LogAnd,
		LogOr,

		// assignment (lhs op= rhs; plain '=' is Assign)
		Assign,
		AddAssign,
		SubAssign,
		MulAssign,
		DivAssign,
		RemAssign,
		ShlAssign,
		ShrAssign,
		AndAssign,
		OrAssign,
		XorAssign,
	};

	enum class ExprKind : U8 {
		IntLit,
		Unary,
		Binary, // also assignment (op in the assignment range)
		Ternary,
		Comma,
	};

	struct Expr {
		ExprKind kind = ExprKind::IntLit;
		U32 offset = 0; // byte offset of the leading token (diagnostics)
		union {
			struct {
				I64 value;
				B32 isUnsigned;
				B32 isLong;
			} intLit;
			struct {
				ExprOp op;
				Expr* operand;
			} unary;
			struct {
				ExprOp op;
				Expr* lhs;
				Expr* rhs;
			} binary;
			struct {
				Expr* cond;
				Expr* whenTrue;
				Expr* whenFalse;
			} ternary;
			struct {
				Expr* lhs;
				Expr* rhs;
			} comma;
		};
	};

	enum class StmtKind : U8 {
		Compound,
		Return,
		Expr,
		Empty,
	};

	struct Stmt {
		StmtKind kind = StmtKind::Empty;
		U32 offset = 0;
		Expr* expr = nullptr;  // Return (may be null), Expr
		List<Stmt*> body;      // Compound only
	};

	enum class TypeSpec : U8 {
		Int,
	};

	struct FuncDef {
		String name;
		TypeSpec retType = TypeSpec::Int;
		Stmt* body = nullptr; // compound statement
		U32 offset = 0;
	};

	struct TransUnit {
		List<FuncDef*> functions;
	};

	constexpr B32 isAssignOp(ExprOp op) {
		return op >= ExprOp::Assign && op <= ExprOp::XorAssign;
	}

	const char* exprOpName(ExprOp op);
	void dumpAst(const TransUnit& unit, std::ostream& os);
} // namespace rat::cc

#endif
