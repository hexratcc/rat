#include "parse/ast.h"

#include <ostream>

namespace rat::cc {
	// one shared { re, im } layout per element width
	const StructType* complexLayout(U32 bits) {
		static const String kReal = "__real";
		static const String kImag = "__imag";
		static Map<U32, StructType> layouts;
		StructType& st = layouts[bits];
		if(st.complete)
			return &st;
		U32 elemBytes = (bits + 7) / 8;
		CType elem = {bits, 0, 0, 0, 0, CType::Base::Float};
		st.size = 2 * elemBytes;
		st.align = elemBytes;
		st.complete = true;
		st.fields.push_back({&kReal, elem, 0});
		st.fields.push_back({&kImag, elem, elemBytes});
		return &st;
	}

	// arr with count as its outer bound
	CType sizedArray(Arena& arena, CType arr, U64 count) {
		if(arr.array->count == count && !isVlaType(arr))
			return arr;
		ArrayType* sized = arena.make<ArrayType>();
		sized->elem = arr.array->elem;
		sized->count = count;
		arr.array = sized;
		return arr;
	}

	CType promote(CType t) {
		if(t.isVoid())
			return t;
		if(t.bitPrec) {
			if(t.bitPrec < 32 || (t.bitPrec == 32 && !t.isUnsigned()))
				return CType{};
			if(t.bitPrec == 32)
				return ctUnsigned(32);
			return t;
		}
		if(t.bits < 32)
			return CType{};
		return t;
	}

	namespace detail {
		U32 intPrec(CType t) { return t.bitPrec ? t.bitPrec : t.bits; }
	} // namespace detail

	CType defaultArgPromote(CType t) {
		if(isFloating(t) && t.bits < 64) {
			t.bits = 64;
			return t;
		}
		return promote(t);
	}

	CType usualArithmetic(CType a, CType b) {
		if(isComplexType(a) || isComplexType(b)) {
			CType ra = isComplexType(a) ? complexElem(a) : a;
			CType rb = isComplexType(b) ? complexElem(b) : b;
			CType r = usualArithmetic(ra, rb);
			r.set(CType::Complex);
			r.base = CType::Base::Float;
			r.strukt = complexLayout(r.bits);
			return r;
		}
		if(isFloating(a) || isFloating(b)) {
			U32 aBits = isFloating(a) ? a.bits : 0;
			U32 bBits = isFloating(b) ? b.bits : 0;
			return {std::max(aBits, bBits), 0, 0, 0, 0, CType::Base::Float};
		}
		a = promote(a);
		b = promote(b);
		if(a.isUnsigned() == b.isUnsigned()) {
			U32 pa = detail::intPrec(a);
			U32 pb = detail::intPrec(b);
			if(pa != pb)
				return pa > pb ? a : b;
			return a.longRank() >= b.longRank() ? a : b; // keep the higher rank
		}
		CType u = a.isUnsigned() ? a : b;
		CType s = a.isUnsigned() ? b : a;
		if(detail::intPrec(u) >= detail::intPrec(s))
			return u;
		return s;
	}

	// t as an rvalue, an array decays to a pointer to its element and a function to a pointer
	CType valueType(CType t) {
		if(isArrayType(t))
			return decay(t);
		if(t.func && t.ptr == 0)
			return pointerTo(t);
		return t;
	}

	// the value of an expression of type t, without its top qualifiers
	CType rvalueType(CType t) {
		t = valueType(t);
		clearTopConst(t);
		clearTopVolatile(t);
		return t;
	}

	// 0 or (void*)0, C99 6.3.2.3p3
	B32 isNullPointerConst(const Expr* e) {
		if(e->kind == ExprKind::Cast && isVoidType(pointee(e->type)) && e->type.ptr == 1)
			e = e->cast.operand;
		return e->kind == ExprKind::IntLit && e->intLit.value == 0;
	}

	// c ? a : b, a null pointer constant takes the type of the other pointer arm
	CType ternaryType(const Expr* whenTrue, const Expr* whenFalse) {
		CType a = rvalueType(whenTrue->type);
		CType b = rvalueType(whenFalse->type);
		if(isPointer(b) && isNullPointerConst(whenTrue))
			return b;
		if(isVoidType(a))
			return a;
		if(isVoidType(b))
			return b;
		if(isComplexType(a) || isComplexType(b))
			return usualArithmetic(a, b);
		if(isStruct(a))
			return a;
		if(isStruct(b))
			return b;
		if(isPointer(a))
			return a;
		if(isPointer(b))
			return b;
		return usualArithmetic(a, b);
	}

	namespace detail {
		B32 funcTypesCompatible(const FuncType* a, const FuncType* b) {
			if(a->isVarArgs != b->isVarArgs || a->params.size() != b->params.size())
				return false;
			if(!typesCompatible(a->ret, b->ret))
				return false;
			for(U32 i = 0; i < a->params.size(); ++i)
				if(!typesCompatible(a->params[i].type, b->params[i].type))
					return false;
			return true;
		}
	} // namespace detail

	// the same type as _Generic sees it, the spelling of long and plain char counts
	B32 typesCompatible(const CType& a, const CType& b) {
		if(a.ptr != b.ptr || a.quals != b.quals || a.vquals != b.vquals)
			return false;
		if((a.func != nullptr) != (b.func != nullptr))
			return false;
		if(a.func)
			return detail::funcTypesCompatible(a.func, b.func);
		if(isArrayType(a) != isArrayType(b))
			return false;
		if(isArrayType(a))
			return a.array->count == b.array->count && typesCompatible(a.array->elem, b.array->elem);
		if((a.strukt != nullptr) != (b.strukt != nullptr))
			return false;
		if(a.strukt)
			return a.strukt == b.strukt;
		if(a.isVoid() != b.isVoid() || a.isFloat() != b.isFloat() || a.bits != b.bits)
			return false;
		if(a.isFloat() || a.isVoid())
			return true;
		if(a.isUnsigned() != b.isUnsigned())
			return false;
		if(a.bits == 8 && a.isPlainChar() != b.isPlainChar())
			return false;
		if(a.bits == 32 && a.isLong() != b.isLong())
			return false;
		return a.bits != 64 || a.isLongLong() == b.isLongLong();
	}

	// the return type of a builtin the emitter knows by name, see emitBuiltinCall
	// abs, labs, llabs and alloca count without the __builtin_ prefix too
	B32 builtinReturnType(const String& name, U32 longBits, CType& out) {
		B32 prefixed = name.rfind("__builtin_", 0) == 0;
		if(!prefixed && name != "abs" && name != "labs" && name != "llabs" && name != "alloca")
			return false;
		String stem = name;
		if(prefixed)
			stem = name.substr(10);
		if(stem == "strlen" || stem == "labs") {
			out = {longBits};
			out.set(CType::Long);
			out.set(CType::Unsigned, stem == "strlen");
			return true;
		}
		constexpr CType kFloat{32, 0, 0, 0, 0, CType::Base::Float};
		constexpr CType kCharPtr{8, 1, 0, 0, 0, CType::Base::Int, CType::PlainChar};
		constexpr CType kLongLong{64, 0, 0, 0, 0, CType::Base::Int, CType::Long | CType::LongLong};
		// clang-format off
		static const Map<String, CType> kTypes = {
				{"sqrtf", kFloat}, {"fabsf", kFloat}, {"copysignf", kFloat},
				{"sqrt", ctDouble()}, {"fabs", ctDouble()}, {"copysign", ctDouble()},
				{"alloca", ctVoidPtr()}, {"calloc", ctVoidPtr()}, {"malloc", ctVoidPtr()},
				{"realloc", ctVoidPtr()}, {"memcpy", ctVoidPtr()}, {"memmove", ctVoidPtr()},
				{"memset", ctVoidPtr()}, {"strcat", kCharPtr}, {"strchr", kCharPtr}, {"strcpy", kCharPtr},
				{"strncat", kCharPtr}, {"strncpy", kCharPtr}, {"strrchr", kCharPtr}, {"strstr", kCharPtr},
				{"abort", ctVoid()}, {"exit", ctVoid()}, {"free", ctVoid()}, {"longjmp", ctVoid()},
				{"expect", ctInt64()}, {"constant_p", ctInt()}, {"abs", ctInt()}, {"llabs", kLongLong},
				{"bswap16", ctUnsigned(16)}, {"bswap32", ctUnsigned(32)}, {"bswap64", ctUnsigned(64)},
				{"va_start", ctVoid()}, {"va_end", ctVoid()}, {"va_copy", ctVoid()},
				{"trap", ctVoid()}, {"unreachable", ctVoid()}, {"prefetch", ctVoid()},
				{"frame_address", ctVoidPtr()}, {"return_address", ctVoidPtr()},
		};
		// clang-format on
		auto found = kTypes.find(stem);
		if(found == kTypes.end())
			return false;
		out = found->second;
		return true;
	}

	namespace detail {
		const C8* floatName(U32 bits) {
			if(bits == 32)
				return "float";
			return bits == 128 ? "long double" : "double";
		}

		const C8* intName(U32 bits) {
			switch(bits) {
			case 8:
				return "char";
			case 16:
				return "short";
			case 32:
				return "int";
			default:
				return "long";
			}
		}

		String funcTypeName(CType t) {
			String s = typeName(t.func->ret) + " (";
			s.append(t.ptr, '*');
			s += ")(";
			for(U32 i = 0; i < t.func->params.size(); ++i) {
				if(i)
					s += ", ";
				s += typeName(t.func->params[i].type);
			}
			if(t.func->isVarArgs)
				s += t.func->params.empty() ? "..." : ", ...";
			else if(t.func->params.empty())
				s += "void";
			s += ")";
			return s;
		}
	} // namespace detail

	String typeName(CType t) {
		if(t.func)
			return detail::funcTypeName(t);
		String s;
		if(t.array)
			s = typeName(t.array->elem) + "[" + std::to_string(t.array->count) + "]";
		else if(t.isComplex())
			s = String(detail::floatName(t.bits)) + " _Complex";
		else if(t.strukt)
			s = (t.strukt->isUnion ? "union " : "struct ") + t.strukt->tag;
		else if(t.isVoid())
			s = "void";
		else if(t.isFloat())
			s = detail::floatName(t.bits);
		else if(t.bits == 1)
			return "_Bool";
		else
			s = String(t.isUnsigned() ? "unsigned " : "") + detail::intName(t.bits);
		s.append(t.ptr, '*');
		return s;
	}

	const C8* exprOpName(ExprOp op) {
		static const C8* const kNames[] = {
				"+",	"-",	"!",	"~",	 "&",		"*",	"__real__", "__imag__", "++", "--", "++",
				"--", "+",	"-",	"*",	 "/",		"%",	"<<",				">>",				"<",	">",	"<=",
				">=", "==", "!=", "&",	 "|",		"^",	"&&",				"||",				"=",	"+=", "-=",
				"*=", "/=", "%=", "<<=", ">>=", "&=", "|=",				"^=",
		};
		static_assert(sizeof(kNames) / sizeof(kNames[0]) == (U32)ExprOp::XorAssign + 1,
									"kNames must cover every ExprOp");
		return kNames[(U32)op];
	}

	namespace detail {
		void pad(std::ostream& os, U32 depth) {
			for(U32 i = 0; i < depth; ++i)
				os << "  ";
		}

		void dumpExpr(const Expr* e, U32 depth, std::ostream& os) {
			pad(os, depth);
			switch(e->kind) {
			case ExprKind::IntLit:
				os << "int " << e->intLit.value;
				if(e->intLit.mods & CType::Unsigned)
					os << "u";
				if(e->intLit.bits == 64)
					os << ((e->intLit.mods & CType::LongLong) ? "ll" : "l");
				os << "\n";
				return;
			case ExprKind::FloatLit:
				os << (e->floatLit.bits == 32 ? "float " : "double ") << e->floatLit.value << "\n";
				return;
			case ExprKind::StrLit:
				os << "str \"" << *e->str.bytes << "\"\n";
				return;
			case ExprKind::Ident:
				os << "ident " << *e->ident.name << "\n";
				return;
			case ExprKind::Call:
				os << "call " << (e->call.callee ? *e->call.callee : "(*)") << "\n";
				if(e->call.target)
					dumpExpr(e->call.target, depth + 1, os);
				for(const Expr* arg : e->args)
					dumpExpr(arg, depth + 1, os);
				return;
			case ExprKind::Cast:
				os << "cast " << typeName(e->cast.type) << "\n";
				dumpExpr(e->cast.operand, depth + 1, os);
				return;
			case ExprKind::Sizeof:
			case ExprKind::AlignOf: {
				const C8* what = e->kind == ExprKind::Sizeof ? "sizeof" : "_Alignof";
				if(e->sizeOf.operand) {
					os << what << "\n";
					dumpExpr(e->sizeOf.operand, depth + 1, os);
				} else {
					os << what << " " << typeName(e->sizeOf.type) << "\n";
				}
			}
				return;
			case ExprKind::Unary:
				os << "unary " << exprOpName(e->unary.op) << "\n";
				dumpExpr(e->unary.operand, depth + 1, os);
				return;
			case ExprKind::Binary:
				os << (isAssignOp(e->binary.op) ? "assign " : "binary ") << exprOpName(e->binary.op)
					 << "\n";
				dumpExpr(e->binary.lhs, depth + 1, os);
				dumpExpr(e->binary.rhs, depth + 1, os);
				return;
			case ExprKind::Ternary:
				os << "ternary ?:\n";
				dumpExpr(e->ternary.cond, depth + 1, os);
				dumpExpr(e->ternary.whenTrue, depth + 1, os);
				dumpExpr(e->ternary.whenFalse, depth + 1, os);
				return;
			case ExprKind::Comma:
				os << "comma\n";
				dumpExpr(e->comma.lhs, depth + 1, os);
				dumpExpr(e->comma.rhs, depth + 1, os);
				return;
			case ExprKind::Member:
				os << "member " << (e->member.arrow ? "->" : ".") << *e->member.name << "\n";
				dumpExpr(e->member.base, depth + 1, os);
				return;
			case ExprKind::InitList:
				os << "initlist\n";
				for(const Expr* el : e->args)
					dumpExpr(el, depth + 1, os);
				return;
			case ExprKind::CompoundLit:
				os << "compound " << typeName(e->compound.type) << "\n";
				dumpExpr(e->compound.init, depth + 1, os);
				return;
			case ExprKind::VaArg:
				os << "va_arg " << typeName(e->vaArg.type) << "\n";
				dumpExpr(e->vaArg.ap, depth + 1, os);
				return;
			case ExprKind::StmtExpr:
				os << "stmt-expr\n";
				dumpStmt(e->stmtExpr.body, depth + 1, os);
				return;
			}
		}

		void dumpStmt(const Stmt* s, U32 depth, std::ostream& os) {
			// clang-format off
			static const C8* const kNames[] = {
					"block", "decl", "if", "while", "do-while", "for", "switch", "case", "default", "break",
					"continue", "return", "expr", "empty", "label", "goto", "asm",
			};
			// clang-format on
			static_assert(sizeof(kNames) / sizeof(kNames[0]) == (U32)StmtKind::Asm + 1,
										"kNames must cover every StmtKind");
			pad(os, depth);
			os << kNames[(U32)s->kind];
			if(s->kind == StmtKind::Label || s->kind == StmtKind::Goto)
				os << " " << *s->label;
			if(s->kind == StmtKind::Asm)
				os << " \"" << *s->asmBlock->text << "\"";
			os << "\n";
			for(const Stmt* child : s->body)
				dumpStmt(child, depth + 1, os);
			for(const Declarator& d : s->decls) {
				pad(os, depth + 1);
				os << "var " << typeName(d.type) << " " << *d.name << "\n";
				if(d.init)
					dumpExpr(d.init, depth + 2, os);
			}
			if(s->kind == StmtKind::DoWhile) {
				dumpStmt(s->thenBody, depth + 1, os);
				dumpExpr(s->expr, depth + 1, os);
				return;
			}
			if(s->forInit)
				dumpStmt(s->forInit, depth + 1, os);
			if(s->expr)
				dumpExpr(s->expr, depth + 1, os);
			if(s->forPost)
				dumpExpr(s->forPost, depth + 1, os);
			if(s->thenBody)
				dumpStmt(s->thenBody, depth + 1, os);
			if(s->elseBody)
				dumpStmt(s->elseBody, depth + 1, os);
		}
	} // namespace detail

	void dumpAst(const TransUnit& unit, std::ostream& os) {
		for(const Stmt* g : unit.globals)
			detail::dumpStmt(g, 0, os);
		for(const FuncDef* fn : unit.functions) {
			os << "func " << fn->name << "(";
			const List<Param>& params = fn->type->params;
			for(U32 i = 0; i < params.size(); ++i) {
				if(i)
					os << ", ";
				os << typeName(params[i].type);
				if(params[i].name)
					os << " " << *params[i].name;
			}
			os << ") -> " << typeName(fn->type->ret) << "\n";
			if(fn->body)
				detail::dumpStmt(fn->body, 1, os);
		}
	}
} // namespace rat::cc
