#ifndef RAT_CC_AST_H
#define RAT_CC_AST_H

#include "Core.h"

namespace rat::cc {
	enum class ExprOp : U8 {
		// unary
		Pos,		// +x
		Neg,		// -x
		Not,		// !x
		BitNot, // ~x
		Addr,		// &x  (address-of)
		Deref,	// *x  (pointer dereference)
		Real,		// __real__ x  (the real part of a complex value, lvalue)
		Imag,		// __imag__ x  (the imaginary part of a complex value, lvalue)

		// increment / decrement (operand is lvalue)
		PreInc,	 // ++x
		PreDec,	 // --x
		PostInc, // x++
		PostDec, // x--

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

		// assignment
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

	struct StructType;
	struct ArrayType;
	struct FuncType;
	struct Expr;

	struct CType {
		U32 bits = 32;
		B32 isUnsigned = false;
		B32 isVoid = false;
		U32 ptr = 0;
		const StructType* strukt = nullptr;
		const ArrayType* array = nullptr;
		B32 isFloat = false;
		B32 isComplex = false;
		const FuncType* func = nullptr;
		U32 quals = 0;
		B32 isPlainChar = false;
		B32 isLongLong = false;
		const Expr* typeofExpr = nullptr;
	};

	constexpr CType ctInt() { return CType{32, false, false, 0}; }
	constexpr B32 isPointer(CType t) { return t.ptr > 0; }
	constexpr B32 isTopConst(CType t) { return (t.quals & (1u << t.ptr)) != 0; }
	constexpr B32 isFloating(CType t) {
		return t.isFloat && !t.isComplex && t.ptr == 0;
	}
	constexpr B32 isComplexType(CType t) { return t.isComplex && t.ptr == 0; }
	constexpr CType complexElem(CType t) {
		t.isComplex = false;
		t.ptr = 0;
		t.strukt = nullptr;
		return t;
	}
	constexpr B32 isStruct(CType t) {
		return t.strukt != nullptr && t.ptr == 0 && !t.isComplex;
	}
	constexpr B32 isCharType(CType t) {
		return t.bits == 8 && t.ptr == 0 && !isStruct(t);
	}
	constexpr B32 isAggregate(CType t) {
		return t.strukt != nullptr && t.ptr == 0;
	}
	constexpr B32 isVoidType(CType t) { return t.isVoid && t.ptr == 0; }
	constexpr B32 isInteger(CType t) {
		return t.ptr == 0 && !t.isFloat && !t.isComplex && !t.isVoid &&
					 t.strukt == nullptr;
	}

	struct Field {
		const String* name = nullptr;
		CType type;
		U32 offset = 0;
		B32 isArray = false;
		U32 count = 0;
		B32 isBitfield = false;
		U32 bitWidth = 0;
		U32 bitOffset = 0;
		B32 anonMember = false;
		B32 anonFirst = false;
		B32 anonUnion = false;
	};

	struct StructType {
		String tag;
		List<Field> fields;
		U32 size = 0;
		U32 align = 1;
		B32 isUnion = false;
		B32 complete = false;

		const Field* find(const String& name) const {
			for (const Field& f : fields)
				if (f.name && *f.name == name)
					return &f;
			return nullptr;
		}
	};

	struct ArrayType {
		CType elem;
		U32 count = 0;
		const Expr* countExpr = nullptr;
	};

	inline B32 isVlaType(CType t) {
		return t.ptr == 0 && t.array != nullptr && t.array->countExpr != nullptr;
	}
	inline B32 hasVlaDim(CType t) {
		for (CType c = t; c.ptr == 0 && c.array != nullptr; c = c.array->elem)
			if (c.array->countExpr != nullptr)
				return true;
		return false;
	}

	struct FuncType {
		CType ret;
		List<CType> params;
		List<const String*> paramNames;
		B32 isVarArgs = false;
		B32 unprototyped = false;
	};

	constexpr B32 isFuncPtr(CType t) { return t.func != nullptr && t.ptr > 0; }
	constexpr B32 isArrayType(CType t) {
		return t.array != nullptr && t.ptr == 0;
	}
	constexpr CType arrayElem(CType t) { return t.array->elem; }
	constexpr CType decay(CType t) {
		CType e = t.array->elem;
		++e.ptr;
		return e;
	}
	constexpr CType pointee(CType t) {
		CType p = t;
		if (p.ptr)
			--p.ptr;
		return p;
	}
	constexpr CType pointerTo(CType t) {
		CType p = t;
		++p.ptr;
		return p;
	}

	constexpr U32 typeSize(CType t, U32 pointerBytes) {
		if (t.ptr > 0)
			return pointerBytes;
		if (isArrayType(t))
			return t.array->count * typeSize(t.array->elem, pointerBytes);
		if (t.strukt)
			return t.strukt->size;
		if (t.isVoid)
			return 1;
		// C99 6.2.5p13: a complex type has the same representation as an array of
		// two of its corresponding real type (real part first).
		if (t.isComplex)
			return 2 * ((t.bits + 7) / 8);
		return (t.bits + 7) / 8;
	}

	enum class ExprKind : U8 {
		IntLit,
		Ident,
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
				const String* name; // arena-owned identifier spelling
			} ident;
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
		Decl,
		Return,
		Expr,
		Empty,
	};

	struct Declarator {
		const String* name = nullptr;
		Expr* init = nullptr; // optional initializer
		U32 offset = 0;
	};

	struct Stmt {
		StmtKind kind = StmtKind::Empty;
		U32 offset = 0;
		Expr* expr = nullptr;     // Return (may be null), Expr
		List<Stmt*> body;         // Compound only
		List<Declarator> decls;   // Decl only
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
