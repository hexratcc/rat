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
	constexpr B32 isFloating(CType t) { return t.isFloat && !t.isComplex && t.ptr == 0; }
	constexpr B32 isComplexType(CType t) { return t.isComplex && t.ptr == 0; }
	constexpr CType complexElem(CType t) {
		t.isComplex = false;
		t.ptr = 0;
		t.strukt = nullptr;
		return t;
	}
	constexpr B32 isStruct(CType t) { return t.strukt != nullptr && t.ptr == 0 && !t.isComplex; }
	constexpr B32 isCharType(CType t) { return t.bits == 8 && t.ptr == 0 && !isStruct(t); }
	constexpr B32 isAggregate(CType t) { return t.strukt != nullptr && t.ptr == 0; }
	constexpr B32 isVoidType(CType t) { return t.isVoid && t.ptr == 0; }
	constexpr B32 isInteger(CType t) {
		return t.ptr == 0 && !t.isFloat && !t.isComplex && !t.isVoid && t.strukt == nullptr;
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
			for(const Field& f : fields)
				if(f.name && *f.name == name)
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
		for(CType c = t; c.ptr == 0 && c.array != nullptr; c = c.array->elem)
			if(c.array->countExpr != nullptr)
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
	constexpr B32 isArrayType(CType t) { return t.array != nullptr && t.ptr == 0; }
	constexpr CType arrayElem(CType t) { return t.array->elem; }
	constexpr CType decay(CType t) {
		CType e = t.array->elem;
		++e.ptr;
		return e;
	}
	constexpr CType pointee(CType t) {
		CType p = t;
		if(p.ptr)
			--p.ptr;
		return p;
	}
	constexpr CType pointerTo(CType t) {
		CType p = t;
		++p.ptr;
		return p;
	}

	constexpr U32 typeSize(CType t, U32 pointerBytes) {
		if(t.ptr > 0)
			return pointerBytes;
		if(isArrayType(t))
			return t.array->count * typeSize(t.array->elem, pointerBytes);
		if(t.strukt)
			return t.strukt->size;
		if(t.isVoid)
			return 1;
		// C99 6.2.5p13: a complex type has the same representation as an array of
		// two of its corresponding real type (real part first).
		if(t.isComplex)
			return 2 * ((t.bits + 7) / 8);
		return (t.bits + 7) / 8;
	}

	enum class ExprKind : U8 {
		IntLit,
		FloatLit,
		StrLit,
		Ident,
		Call,
		Cast,
		Sizeof,
		Unary,
		Binary,
		Ternary,
		Comma,
		Member,			 // s.field or p->field
		InitList,		 // { a, b, c } brace initializer
		CompoundLit, // (type){ ... } C99 compound literal
		VaArg,			 // __builtin_va_arg(ap, type)
		StmtExpr,		 // GNU statement expression ({ stmts; value; })
		Generic,		 // C11 _Generic(ctrl, type: expr, ..., default: expr)
	};

	struct Expr;

	struct GenericAssoc {
		B32 isDefault = false;
		CType type;
		Expr* result = nullptr;
	};

	struct Designator {
		B32 isSet = false;
		B32 isIndex = false;
		I64 index = 0;
		const String* field = nullptr;
		const Designator* next = nullptr;
	};

	struct Stmt;

	struct Expr {
		Expr() {}

		ExprKind kind = ExprKind::IntLit;
		U32 offset = 0; // byte offset of the leading token (diagnostics)
		union {
			struct {
				I64 value;
				B32 isUnsigned;
				B32 isLong;
			} intLit;
			struct {
				long double value;
				B32 isFloat;
				B32 isLongDouble;
				B32 isImaginary;
			} floatLit;
			struct {
				const String* bytes;
				B32 isWide;
				U32 charSize;
			} str;
			struct {
				const String* name; // arena-owned identifier spelling
			} ident;
			struct {
				const String* callee;
				Expr* target;
			} call;
			struct {
				CType type;
				Expr* operand;
			} cast;
			struct {
				CType type;
				Expr* operand;
			} sizeOf;
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
			struct {
				Expr* base;
				const String* name;
				B32 arrow;
			} member;
			struct {
				CType type;
				Expr* init;
				Expr* arrayLen;
				B32 isArray;
			} compound;
			struct {
				Expr* ap;
				CType type;
			} vaArg;
			struct {
				Stmt* body;
			} stmtExpr;
			struct {
				Expr* control;
			} generic;
		};
		List<Expr*> args;
		List<Designator> designators;
		List<GenericAssoc> assocs;
	};

	enum class StmtKind : U8 {
		Compound,
		Decl,
		If,
		While,
		DoWhile,
		For,
		Switch,
		Case,
		Default,
		Break,
		Continue,
		Return,
		Expr,
		Empty,
		Label,
		Goto,
	};

	struct Declarator {
		const String* name = nullptr;
		Expr* init = nullptr; // optional initializer
		CType type = ctInt();
		B32 isArray = false;
		Expr* arrayLen = nullptr;
		B32 isStatic = false;
		B32 isExtern = false;
		U32 offset = 0;
	};

	struct Stmt {
		StmtKind kind = StmtKind::Empty;
		U32 offset = 0;
		Expr* expr = nullptr;
		List<Stmt*> body;
		List<Declarator> decls;
		Stmt* thenBody = nullptr;
		Stmt* elseBody = nullptr;
		Stmt* forInit = nullptr;
		Expr* forPost = nullptr;
		const String* label = nullptr;
	};

	CType promote(CType t);
	CType defaultArgPromote(CType t);
	CType usualArithmetic(CType a, CType b);
	String typeName(CType t);

	struct Param {
		const String* name = nullptr;
		CType type = ctInt();
		U32 offset = 0;
		const Expr* vlaBound = nullptr;
	};

	struct FuncDef {
		String name;
		CType retType = ctInt();
		List<Param> params;
		B32 isVarArgs = false;
		B32 unprototyped = false;
		B32 isExternInline = false;
		B32 isStatic = false;
		Stmt* body = nullptr; // compound statement
		U32 offset = 0;
	};

	struct TransUnit {
		List<FuncDef*> functions;
		List<Stmt*> globals;
	};

	constexpr B32 isAssignOp(ExprOp op) { return op >= ExprOp::Assign && op <= ExprOp::XorAssign; }

	constexpr B32 compoundBaseOp(ExprOp op, ExprOp& base) {
		if(op < ExprOp::AddAssign || op > ExprOp::XorAssign)
			return false;
		// clang-format off
		constexpr ExprOp kBase[] = {
				ExprOp::Add, ExprOp::Sub, ExprOp::Mul,    ExprOp::Div,   ExprOp::Rem,
				ExprOp::Shl, ExprOp::Shr, ExprOp::BitAnd, ExprOp::BitOr, ExprOp::BitXor,
		};
		// clang-format on
		static_assert(sizeof(kBase) / sizeof(kBase[0]) ==
											(U32)ExprOp::XorAssign - (U32)ExprOp::AddAssign + 1,
									"kBase must cover AddAssign..XorAssign");
		base = kBase[(U32)op - (U32)ExprOp::AddAssign];
		return true;
	}

	const char* exprOpName(ExprOp op);
	void dumpAst(const TransUnit& unit, std::ostream& os);
} // namespace rat::cc

#endif
