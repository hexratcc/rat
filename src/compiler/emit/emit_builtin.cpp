#include "emit/emit.h"

namespace rat::cc {
	static U32 bitBuiltinWidth(const String& name, const C8* stem, U32 longBits) {
		String prefix = String("__builtin_") + stem;
		if(name.size() < prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
			return 0;
		String suffix = name.substr(prefix.size());
		if(suffix.empty())
			return 32;
		if(suffix == "l")
			return longBits;
		if(suffix == "ll")
			return 64;
		return 0;
	}

	static B32 bitBuiltinSuffix(const String& name, const C8* stem) {
		String prefix = String("__builtin_") + stem;
		if(name.size() < prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
			return false;
		String suffix = name.substr(prefix.size());
		return suffix.empty() || suffix == "f" || suffix == "l";
	}

	static CType ctUnsigned(U32 bits) {
		CType t;
		t.bits = bits;
		t.set(CType::Unsigned);
		return t;
	}

	static CType ctVoid() {
		CType t;
		t.base = CType::Base::Void;
		return t;
	}

	static CType ctVoidPtr() {
		CType t = ctVoid();
		t.ptr = 1;
		return t;
	}

	static CType ctDouble() {
		CType t;
		t.base = CType::Base::Float;
		t.bits = 64;
		return t;
	}

	static F64 f64Inf() {
		U64 bits = 0x7ff0000000000000ull;
		F64 v;
		std::memcpy(&v, &bits, sizeof(v));
		return v;
	}

	I64 Emitter::typeClassOf(CType t) {
		if(isPointer(t) || isArrayType(t) || t.func != nullptr)
			return 5; // pointer_type_class
		if(isVoidType(t))
			return 0; // void_type_class
		if(isComplexType(t))
			return 9; // complex_type_class
		if(isStruct(t))
			return t.strukt->isUnion ? 13 : 12; // union_ / record_type_class
		if(isFloating(t))
			return 8; // real_type_class
		return 1;		// integer_type_class
	}

	static const C8* const kLibcBuiltins[] = {
			"abort",		"abs",		 "alloca",	"calloc",	 "copysign", "copysignf", "exit",		 "fabs",
			"fabsf",		"fprintf", "free",		"labs",		 "llabs",		 "longjmp",		"malloc",	 "memcmp",
			"memcpy",		"memmove", "memset",	"printf",	 "putchar",	 "puts",			"realloc", "setjmp",
			"snprintf", "sprintf", "sqrt",		"sqrtf",	 "strcat",	 "strchr",		"strcmp",	 "strcpy",
			"strlen",		"strncat", "strncmp", "strncpy", "strrchr",	 "strstr",
	};
	static const C8* const kUnimplementedBuiltins[] = {
			"apply",
			"apply_args",
			"assume_aligned",
			"extract_return_addr",
			"va_arg_pack",
			"va_arg_pack_len",
	};

	static B32 nameIn(const C8* const* table, U32 count, const String& stem) {
		for(U32 i = 0; i < count; ++i)
			if(stem == table[i])
				return true;
		return false;
	}

	String builtinLibcName(const String& name) {
		if(name.rfind("__builtin_", 0) != 0)
			return String();
		String stem = name.substr(10);
		if(nameIn(kLibcBuiltins, (U32)(sizeof(kLibcBuiltins) / sizeof(kLibcBuiltins[0])), stem))
			return stem;
		return String();
	}

	B32 builtinReturnType(const String& name, U32 longBits, CType& out) {
		if(name.rfind("__builtin_", 0) != 0)
			return false;
		String stem = name.substr(10);
		out = CType{};
		if(stem == "sqrt" || stem == "fabs" || stem == "copysign") {
			out.base = CType::Base::Float;
			out.bits = 64;
			return true;
		}
		if(stem == "sqrtf" || stem == "fabsf" || stem == "copysignf") {
			out.base = CType::Base::Float;
			out.bits = 32;
			return true;
		}
		if(stem == "alloca" || stem == "calloc" || stem == "malloc" || stem == "realloc" ||
			 stem == "memcpy" || stem == "memmove" || stem == "memset") {
			out.base = CType::Base::Void;
			out.ptr = 1;
			return true;
		}
		if(stem == "strcat" || stem == "strchr" || stem == "strcpy" || stem == "strncat" ||
			 stem == "strncpy" || stem == "strrchr" || stem == "strstr") {
			out.bits = 8;
			out.ptr = 1;
			out.set(CType::PlainChar);
			return true;
		}
		if(stem == "strlen") {
			out.bits = longBits;
			out.set(CType::Unsigned);
			out.set(CType::Long);
			return true;
		}
		if(stem == "abort" || stem == "exit" || stem == "free" || stem == "longjmp") {
			out.base = CType::Base::Void;
			return true;
		}
		return false;
	}

	static B32 isKnownBuiltin(const String& name) {
		String stem = name.substr(10);
		return nameIn(kLibcBuiltins, (U32)(sizeof(kLibcBuiltins) / sizeof(kLibcBuiltins[0])), stem) ||
					 nameIn(kUnimplementedBuiltins,
									(U32)(sizeof(kUnimplementedBuiltins) / sizeof(kUnimplementedBuiltins[0])),
									stem);
	}

	Node* Emitter::vaListRef(Function& fn, const Expr* ap) {
		if(!lay.win64VaList)
			return emitExpr(fn, ap).node;
		LValue lv;
		if(!emitLValue(fn, ap, lv))
			return nullptr;
		if(lv.isVar() || !lv.addr) {
			fail("va_list operand must be addressable");
			return nullptr;
		}
		return lv.addr;
	}

	B32 Emitter::emitBitCountBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		Opcode op = Opcode::Clz;
		B32 isFfs = false;
		U32 bits = bitBuiltinWidth(b, "clz", lay.longBits);
		if(bits == 0) {
			bits = bitBuiltinWidth(b, "ctz", lay.longBits);
			op = Opcode::Ctz;
		}
		if(bits == 0) {
			bits = bitBuiltinWidth(b, "popcount", lay.longBits);
			op = Opcode::Popcnt;
		}
		if(bits == 0) {
			bits = bitBuiltinWidth(b, "ffs", lay.longBits);
			op = Opcode::Ctz;
			isFfs = bits != 0;
		}
		if(bits == 0)
			return false;
		if(e->args.size() != 1) {
			fail("'" + b + "' expects one argument");
			return true;
		}
		Value a = emitExpr(fn, e->args[0]);
		if(!a.node)
			return true;
		CType ut = ctUnsigned(bits);
		Type* ty = irType(ut);
		Node* x = convert(fn, a.node, a.type, ut);
		if(!isFfs) {
			out = {convert(fn, fn.unary(op, x), ut, ctInt()), ctInt()};
			return true;
		}
		Node* top = fn.constInt(ty, (I64)((U64)1 << (bits - 1)));
		Node* idx = fn.add(fn.ctz(fn.or_(x, top)), fn.constInt(ty, 1));
		Node* nonZero = fromBool(fn, fn.ne(x, fn.constInt(ty, 0)));
		out = {fn.mul(convert(fn, idx, ut, ctInt()), nonZero), ctInt()};
		return true;
	}

	B32 Emitter::emitAbsBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& name = *e->call.callee;
		String stem = name.rfind("__builtin_", 0) == 0 ? name.substr(10) : name;
		U32 bits;
		if(stem == "abs")
			bits = 32;
		else if(stem == "labs")
			bits = lay.longBits;
		else if(stem == "llabs")
			bits = 64;
		else
			return false;
		CType at;
		if(e->args.size() != 1 || !typeOf(e->args[0], at) || !isInteger(at))
			return false;
		Value v = emitExpr(fn, e->args[0]);
		if(!v.node)
			return true;
		CType rt;
		rt.bits = bits;
		rt.set(CType::Long, stem != "abs");
		rt.set(CType::LongLong, stem == "llabs");
		Type* ty = irType(rt);
		Node* x = convert(fn, v.node, v.type, rt);
		Node* mask = fn.ashr(x, fn.constInt(ty, (I64)bits - 1));
		out = {fn.sub(fn.binary(Opcode::Xor, x, mask), mask), rt};
		return true;
	}

	B32 Emitter::emitBswapBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		U32 bits = 0;
		if(b == "__builtin_bswap16")
			bits = 16;
		else if(b == "__builtin_bswap32")
			bits = 32;
		else if(b == "__builtin_bswap64")
			bits = 64;
		else
			return false;
		if(e->args.size() != 1) {
			fail("'" + b + "' expects one argument");
			return true;
		}
		Value a = emitExpr(fn, e->args[0]);
		if(!a.node)
			return true;
		CType wt = ctUnsigned(bits == 16 ? 32 : bits);
		Node* r = fn.bswap(convert(fn, a.node, a.type, wt));
		if(bits == 16)
			r = fn.lshr(r, fn.constInt(irType(wt), 16));
		CType rt = ctUnsigned(bits);
		out = {convert(fn, r, wt, rt), rt};
		return true;
	}

	B32 Emitter::emitClassifyBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		if(b != "__builtin_classify_type")
			return false;
		if(e->args.size() != 1) {
			fail("'" + b + "' expects one argument");
			return true;
		}
		CType t;
		if(!typeOf(e->args[0], t)) {
			fail("cannot classify the operand of '" + b + "'");
			return true;
		}
		out = {fn.constInt(i32, typeClassOf(t)), ctInt()};
		return true;
	}

	B32 Emitter::emitFpClassBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		enum Kind : U8 { Sign, IsInf, IsNan, IsFinite } kind;
		if(bitBuiltinSuffix(b, "signbit"))
			kind = Sign;
		else if(bitBuiltinSuffix(b, "isinf"))
			kind = IsInf;
		else if(bitBuiltinSuffix(b, "isnan"))
			kind = IsNan;
		else if(bitBuiltinSuffix(b, "isfinite"))
			kind = IsFinite;
		else
			return false;
		if(e->args.size() != 1) {
			fail("'" + b + "' expects one argument");
			return true;
		}
		Value a = emitExpr(fn, e->args[0]);
		if(!a.node)
			return true;
		CType ft = isFloating(a.type) ? a.type : ctDouble();
		Node* x = convert(fn, a.node, a.type, ft);
		Type* ty = irType(ft);
		if(kind == Sign) {
			U32 sz = ft.bits == 128 ? 16u : ft.bits / 8;
			U32 off = ft.bits == 128 ? 8u : 0u;
			CType it = ctUnsigned(ft.bits == 128 ? 16u : ft.bits);
			Node* slot = allocBytes(fn, sz);
			fn.store(slot, x);
			Node* raw = fn.load(irType(it), offsetPtr(fn, slot, off));
			Node* s = fn.lshr(raw, fn.constInt(irType(it), (I64)it.bits - 1));
			out = {convert(fn, s, it, ctInt()), ctInt()};
			return true;
		}
		if(kind == IsNan) {
			out = {fromBool(fn, fn.compare(Opcode::FNe, x, x)), ctInt()};
			return true;
		}
		Node* pos = fn.constFloat(ty, f64Inf());
		Node* neg = fn.constFloat(ty, -f64Inf());
		if(kind == IsInf) {
			Node* hi = fromBool(fn, fn.compare(Opcode::FEq, x, pos));
			Node* lo = fromBool(fn, fn.compare(Opcode::FEq, x, neg));
			out = {fn.or_(hi, lo), ctInt()};
			return true;
		}
		// finite: strictly inside the infinities, which a NaN never is
		Node* hi = fromBool(fn, fn.compare(Opcode::FLt, x, pos));
		Node* lo = fromBool(fn, fn.compare(Opcode::FGt, x, neg));
		out = {fn.and_(hi, lo), ctInt()};
		return true;
	}

	B32 Emitter::emitFrameBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		if(b != "__builtin_frame_address" && b != "__builtin_return_address")
			return false;
		if(e->args.size() != 1) {
			fail("'" + b + "' expects one argument");
			return true;
		}
		I64 level = 0;
		if(!evalConst(e->args[0], level)) {
			fail("'" + b + "' expects a constant level");
			return true;
		}
		if(level != 0) {
			out = {fn.constInt(mod.getPtr(), 0), ctVoidPtr()};
			return true;
		}
		List<Node*> args;
		out = {fn.call(b, mod.getPtr(), args, false), ctVoidPtr()};
		return true;
	}

	B32 Emitter::emitPrefetchBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		if(b != "__builtin_prefetch")
			return false;
		if(e->args.empty()) {
			fail("'" + b + "' expects at least one argument");
			return true;
		}
		Value a = emitExpr(fn, e->args[0]);
		if(!a.node)
			return true;
		I64 rw = 0, loc = 3;
		if(e->args.size() > 1)
			evalConst(e->args[1], rw);
		if(e->args.size() > 2)
			evalConst(e->args[2], loc);
		List<Node*> args = {
				convert(fn, a.node, a.type, ctVoidPtr()), fn.constInt(i32, rw), fn.constInt(i32, loc)};
		fn.call(b, nullptr, args, false);
		out = {fn.constInt(i32, 0), ctVoid()};
		return true;
	}

	B32 Emitter::emitBuiltinCall(Function& fn, const Expr* e, Value& out) {
		if(!e->call.callee)
			return false;
		const String& b = *e->call.callee;

		if(b == "__builtin_va_start" || b == "__builtin_va_end") {
			List<Node*> args;
			for(U32 i = 0; i < e->args.size(); ++i) {
				Node* n = i == 0 ? vaListRef(fn, e->args[i]) : emitExpr(fn, e->args[i]).node;
				if(!n)
					return true;
				args.push_back(n);
			}
			fn.call(b, nullptr, args);
			CType v;
			v.base = CType::Base::Void;
			out = {fn.constInt(i32, 0), v};
			return true;
		}

		if(b == "__builtin_va_copy") {
			if(e->args.size() != 2) {
				fail("__builtin_va_copy expects two arguments");
				return true;
			}
			Node* dst = vaListRef(fn, e->args[0]);
			if(!dst)
				return true;
			if(lay.win64VaList) {
				Value src = emitExpr(fn, e->args[1]);
				if(!src.node)
					return true;
				fn.store(dst, src.node);
			} else {
				Value src = emitExpr(fn, e->args[1]);
				if(!src.node)
					return true;
				emitMemCopy(fn, dst, src.node, lay.ptrBytes * 3);
			}
			CType v;
			v.base = CType::Base::Void;
			out = {fn.constInt(i32, 0), v};
			return true;
		}

		if(b == "__builtin_expect") {
			if(e->args.size() != 2) {
				fail("__builtin_expect expects two arguments");
				return true;
			}
			Value exp = emitExpr(fn, e->args[0]);
			if(!exp.node)
				return true;
			Value hint = emitExpr(fn, e->args[1]);
			if(!hint.node)
				return true;
			CType lng;
			lng.bits = 64;
			out = {convert(fn, exp.node, exp.type, lng), lng};
			return true;
		}
		if(b == "__builtin_constant_p") {
			if(e->args.size() != 1) {
				fail("__builtin_constant_p expects one argument");
				return true;
			}
			I64 v = 0;
			B32 isConst = e->args[0]->kind == ExprKind::StrLit || evalConst(e->args[0], v);
			out = {fn.constInt(i32, isConst ? 1 : 0), ctInt()};
			return true;
		}
		if(b == "__builtin_unreachable") {
			CType vd;
			vd.base = CType::Base::Void;
			out = {fn.constInt(i32, 0), vd};
			return true;
		}
		if(b == "__builtin_trap") {
			List<Node*> args;
			fn.call(b, nullptr, args, false);
			out = {fn.constInt(i32, 0), ctVoid()};
			return true;
		}
		if(emitBitCountBuiltin(fn, e, out))
			return true;
		if(emitOverflowBuiltin(fn, e, out))
			return true;
		if(emitAbsBuiltin(fn, e, out))
			return true;
		if(emitBswapBuiltin(fn, e, out))
			return true;
		if(emitClassifyBuiltin(fn, e, out))
			return true;
		if(emitFpClassBuiltin(fn, e, out))
			return true;
		if(emitFrameBuiltin(fn, e, out))
			return true;
		if(emitPrefetchBuiltin(fn, e, out))
			return true;
		if(b.rfind("__builtin_", 0) == 0 && !isKnownBuiltin(b)) {
			fail("unsupported builtin '" + b + "'");
			return true;
		}
		return false;
	}

} // namespace rat::cc
