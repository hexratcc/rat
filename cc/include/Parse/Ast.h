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
		enum class Base : U8 { Int, Float, Void };
		enum Mods : U8 {
			Unsigned = 1u << 0,	 // unsigned integer
			PlainChar = 1u << 1, // 'char' spelled without an explicit sign
			Long = 1u << 2,			 // spelled with 'long'
			LongLong = 1u << 3,	 // spelled with 'long long'
			Complex = 1u << 4,	 // _Complex over Base::Float
		};

		U32 bits = 32;
		U32 ptr = 0;
		U32 quals = 0;
		Base base = Base::Int;
		U8 mods = 0;
		const StructType* strukt = nullptr;
		const ArrayType* array = nullptr;
		const FuncType* func = nullptr;
		const Expr* typeofExpr = nullptr;

		constexpr B32 has(Mods m) const { return (mods & (U8)m) != 0; }
		constexpr void set(Mods m, B32 on = true) {
			mods = on ? (U8)(mods | (U8)m) : (U8)(mods & ~(U8)m);
		}
		constexpr B32 isUnsigned() const { return has(Unsigned); }
		constexpr B32 isPlainChar() const { return has(PlainChar); }
		constexpr B32 isLong() const { return has(Long); }
		constexpr B32 isLongLong() const { return has(LongLong); }
		constexpr B32 isComplex() const { return has(Complex); }
		constexpr B32 isFloat() const { return base == Base::Float; }
		constexpr B32 isVoid() const { return base == Base::Void; }
	};

	constexpr CType ctInt() { return CType{}; }
	constexpr B32 isPointer(CType t) { return t.ptr > 0; }
	constexpr B32 isTopConst(CType t) { return (t.quals & (1u << t.ptr)) != 0; }
	constexpr void setTopConst(CType& t) { t.quals |= (1u << t.ptr); }
	constexpr void clearTopConst(CType& t) { t.quals &= ~(1u << t.ptr); }
	constexpr B32 isFloating(CType t) { return t.isFloat() && !t.isComplex() && t.ptr == 0; }
	constexpr B32 isComplexType(CType t) { return t.isComplex() && t.ptr == 0; }
	constexpr CType complexElem(CType t) {
		t.set(CType::Complex, false);
		t.ptr = 0;
		t.strukt = nullptr;
		return t;
	}
	constexpr B32 isStruct(CType t) { return t.strukt != nullptr && t.ptr == 0 && !t.isComplex(); }
	constexpr B32 isCharType(CType t) { return t.bits == 8 && t.ptr == 0 && !isStruct(t); }
	constexpr B32 isAggregate(CType t) { return t.strukt != nullptr && t.ptr == 0; }
	constexpr B32 isVoidType(CType t) { return t.isVoid() && t.ptr == 0; }
	constexpr B32 isInteger(CType t) {
		return t.ptr == 0 && t.base == CType::Base::Int && !t.isComplex() && t.strukt == nullptr;
	}

	struct Field {
		enum Mods : U8 {
			Array = 1u << 0,		   // count is the element count
			Bitfield = 1u << 1,	   // bitWidth/bitOffset are meaningful
			AnonMember = 1u << 2,  // lifted out of an anonymous struct/union
			AnonFirst = 1u << 3,	 // first field of its anonymous group
			AnonUnion = 1u << 4,	 // the anonymous group is a union
		};

		const String* name = nullptr;
		CType type;
		U32 offset = 0;
		U32 count = 0;
		U32 bitWidth = 0;
		U32 bitOffset = 0;
		U8 mods = 0;

		constexpr B32 has(Mods m) const { return (mods & (U8)m) != 0; }
		constexpr void set(Mods m, B32 on = true) {
			mods = on ? (U8)(mods | (U8)m) : (U8)(mods & ~(U8)m);
		}
		constexpr B32 isArray() const { return has(Array); }
		constexpr B32 isBitfield() const { return has(Bitfield); }
		constexpr B32 anonMember() const { return has(AnonMember); }
		constexpr B32 anonFirst() const { return has(AnonFirst); }
		constexpr B32 anonUnion() const { return has(AnonUnion); }
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

	StructType* makeComplexLayout(Arena& arena, CType complexType);

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
		if(t.isVoid())
			return 1;
		// C99 6.2.5p13: a complex type has the same representation as an array of
		// two of its corresponding real type (real part first).
		if(t.isComplex())
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
				U32 bits;
				U8 mods;
			} intLit;
			struct {
				long double value;
				U32 bits;
				B32 imaginary;
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
