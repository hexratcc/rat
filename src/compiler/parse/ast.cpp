#include "parse/ast.h"

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
} // namespace rat::cc
