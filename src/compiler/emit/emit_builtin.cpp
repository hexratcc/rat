#include "emit/emit.h"

namespace rat::cc {
	namespace detail {
		U32 bitBuiltinWidth(const String& name, const C8* stem, U32 longBits) {
			String prefix = String("__builtin_") + stem;
			if(name.rfind(prefix, 0) != 0)
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

		// true for __builtin_<stem> with no suffix or the f or l suffix
		B32 fpBuiltinSuffix(const String& name, const C8* stem) {
			String prefix = String("__builtin_") + stem;
			if(name.rfind(prefix, 0) != 0)
				return false;
			String suffix = name.substr(prefix.size());
			return suffix.empty() || suffix == "f" || suffix == "l";
		}

		F64 f64Inf() {
			U64 bits = 0x7ff0000000000000ull;
			F64 v;
			std::memcpy(&v, &bits, sizeof(v));
			return v;
		}

		const C8* const kLibcBuiltins[] = {
				"abort",		"abs",		 "calloc",	"copysign", "copysignf", "exit",		"fabs",
				"fabsf",		"fprintf", "free",		"labs",			"llabs",		 "malloc",	"memcmp",
				"memcpy",		"memmove", "memset",	"printf",		"putchar",	 "puts",		"realloc",
				"snprintf", "sprintf", "sqrt",		"sqrtf",		"strcat",		 "strchr",	"strcmp",
				"strcpy",		"strlen",	 "strncat", "strncmp",	"strncpy",	 "strrchr", "strstr",
		};
		const C8* const kUnimplementedBuiltins[] = {
				"apply",
				"apply_args",
				"assume_aligned",
				"extract_return_addr",
				"va_arg_pack",
				"va_arg_pack_len",
		};

		B32 isKnownBuiltin(const String& name) {
			String stem = name.substr(10);
			const C8* const* unimplementedEnd = std::end(kUnimplementedBuiltins);
			return !builtinLibcName(name).empty() ||
						 std::find(kUnimplementedBuiltins, unimplementedEnd, stem) != unimplementedEnd;
		}
	} // namespace detail

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

	String builtinLibcName(const String& name) {
		if(name.rfind("__builtin_", 0) != 0)
			return String();
		String stem = name.substr(10);
		const C8* const* libcEnd = std::end(detail::kLibcBuiltins);
		if(std::find(detail::kLibcBuiltins, libcEnd, stem) != libcEnd)
			return stem;
		return String();
	}

	B32 builtinReturnType(const String& name, U32 longBits, CType& out) {
		if(name.rfind("__builtin_", 0) != 0)
			return false;
		String stem = name.substr(10);
		if(stem == "strlen") {
			out = ctUnsigned(longBits);
			out.set(CType::Long);
			return true;
		}
		constexpr CType kFloat{32, 0, 0, 0, 0, CType::Base::Float};
		constexpr CType kCharPtr{8, 1, 0, 0, 0, CType::Base::Int, CType::PlainChar};
		// clang-format off
		static const Map<String, CType> kTypes = {
				{"sqrtf", kFloat}, {"fabsf", kFloat}, {"copysignf", kFloat},
				{"sqrt", ctDouble()}, {"fabs", ctDouble()}, {"copysign", ctDouble()},
				{"alloca", ctVoidPtr()}, {"calloc", ctVoidPtr()}, {"malloc", ctVoidPtr()},
				{"realloc", ctVoidPtr()}, {"memcpy", ctVoidPtr()}, {"memmove", ctVoidPtr()},
				{"memset", ctVoidPtr()}, {"strcat", kCharPtr}, {"strchr", kCharPtr}, {"strcpy", kCharPtr},
				{"strncat", kCharPtr}, {"strncpy", kCharPtr}, {"strrchr", kCharPtr}, {"strstr", kCharPtr},
				{"abort", ctVoid()}, {"exit", ctVoid()}, {"free", ctVoid()}, {"longjmp", ctVoid()},
		};
		// clang-format on
		auto found = kTypes.find(stem);
		if(found == kTypes.end())
			return false;
		out = found->second;
		return true;
	}

	Emitter::Value Emitter::voidValue(Function& fn) { return {fn.constInt(i32, 0), ctVoid()}; }

	Node* Emitter::vaListRef(Function& fn, const Expr* ap) {
		if(!lay.win64VaList)
			return emitExpr(fn, ap).node;
		LValue lv;
		if(!emitLValue(fn, ap, lv))
			return nullptr;
		if(lv.isVar() || !lv.addr)
			return diag.fail("va_list operand must be addressable");
		return lv.addr;
	}

	B32 Emitter::oneArg(const Expr* e) {
		if(e->args.size() == 1)
			return true;
		return diag.fail("'" + *e->call.callee + "' expects one argument");
	}

	B32 Emitter::emitVaBuiltin(Function& fn, const Expr* e, Value& out) {
		List<Node*> args;
		for(U32 i = 0; i < e->args.size(); ++i) {
			Node* n = i == 0 ? vaListRef(fn, e->args[i]) : emitExpr(fn, e->args[i]).node;
			if(!n)
				return true;
			args.push_back(n);
		}
		fn.call(*e->call.callee, nullptr, args);
		out = voidValue(fn);
		return true;
	}

	B32 Emitter::emitVaCopyBuiltin(Function& fn, const Expr* e, Value& out) {
		if(e->args.size() != 2) {
			diag.fail("__builtin_va_copy expects two arguments");
			return true;
		}
		Node* dst = vaListRef(fn, e->args[0]);
		if(!dst)
			return true;
		Value src = emitExpr(fn, e->args[1]);
		if(!src.node)
			return true;
		if(lay.win64VaList)
			fn.store(dst, src.node);
		else
			emitMemCopy(fn, dst, src.node, lay.ptrBytes * 3);
		out = voidValue(fn);
		return true;
	}

	B32 Emitter::emitAllocaBuiltin(Function& fn, const Expr* e, Value& out) {
		if(!oneArg(e))
			return true;
		Value n = emitExpr(fn, e->args[0]);
		if(!n.node)
			return true;
		func.sawAlloca = true;
		out = {fn.stackAlloc(convert(fn, n.node, n.type, ctSize())), ctVoidPtr()};
		return true;
	}

	B32 Emitter::emitJmpBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& name = *e->call.callee;
		B32 isSet = name == "__builtin_setjmp";
		if(e->args.size() != (isSet ? 1u : 2u)) {
			diag.fail("'" + name + "' expects " + (isSet ? "one argument" : "two arguments"));
			return true;
		}
		Value buf = emitExpr(fn, e->args[0]);
		if(!buf.node)
			return true;
		if(isSet) {
			out = {fn.call(name, i32, {buf.node}, false), ctInt()};
			return true;
		}
		fn.call(name, nullptr, {buf.node}, false); // the value is always 1
		out = voidValue(fn);
		return true;
	}

	B32 Emitter::emitExpectBuiltin(Function& fn, const Expr* e, Value& out) {
		if(e->args.size() != 2) {
			diag.fail("__builtin_expect expects two arguments");
			return true;
		}
		Value exp = emitExpr(fn, e->args[0]);
		if(!exp.node)
			return true;
		Value hint = emitExpr(fn, e->args[1]);
		if(!hint.node)
			return true;
		out = {convert(fn, exp.node, exp.type, ctInt64()), ctInt64()};
		return true;
	}

	B32 Emitter::emitConstantPBuiltin(Function& fn, const Expr* e, Value& out) {
		if(e->args.size() != 1) {
			diag.fail("__builtin_constant_p expects one argument");
			return true;
		}
		I64 v = 0;
		B32 isConst = e->args[0]->kind == ExprKind::StrLit || evalConst(e->args[0], v);
		out = {fn.constInt(i32, isConst ? 1 : 0), ctInt()};
		return true;
	}

	// __builtin_trap and __builtin_unreachable
	B32 Emitter::emitTrapBuiltin(Function& fn, const Expr* e, Value& out) {
		if(*e->call.callee == "__builtin_trap")
			fn.call(*e->call.callee, nullptr, {}, false);
		out = voidValue(fn);
		return true;
	}

	B32 Emitter::emitBitCountBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		struct Probe {
			const C8* stem;
			Opcode op;
		};
		static const Probe kProbes[] = {{"clz", Opcode::Clz},
																		{"ctz", Opcode::Ctz},
																		{"popcount", Opcode::Popcnt},
																		{"ffs", Opcode::Ctz}};
		Opcode op = Opcode::Clz;
		B32 isFfs = false;
		U32 bits = 0;
		for(U32 i = 0; i < 4 && bits == 0; ++i) {
			bits = detail::bitBuiltinWidth(b, kProbes[i].stem, lay.longBits);
			op = kProbes[i].op;
			isFfs = i == 3 && bits != 0; // ffs is the last probe
		}
		if(bits == 0)
			return false;
		if(!oneArg(e))
			return true;
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

	// false when the operand is not an integer, the call then goes to libc
	B32 Emitter::emitAbsBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& name = *e->call.callee;
		String stem = name.rfind("__builtin_", 0) == 0 ? name.substr(10) : name;
		CType at;
		if(e->args.size() != 1 || !typeOf(e->args[0], at) || !isInteger(at))
			return false;
		Value v = emitExpr(fn, e->args[0]);
		if(!v.node)
			return true;
		CType rt;
		if(stem == "labs")
			rt.bits = lay.longBits;
		else if(stem == "llabs")
			rt.bits = 64;
		rt.set(CType::Long, stem != "abs");
		rt.set(CType::LongLong, stem == "llabs");
		Type* ty = irType(rt);
		Node* x = convert(fn, v.node, v.type, rt);
		Node* mask = fn.ashr(x, fn.constInt(ty, (I64)rt.bits - 1));
		out = {fn.sub(fn.binary(Opcode::Xor, x, mask), mask), rt};
		return true;
	}

	B32 Emitter::emitBswapBuiltin(Function& fn, const Expr* e, Value& out) {
		U32 bits = (U32)std::stoul(e->call.callee->substr(sizeof("__builtin_bswap") - 1));
		if(!oneArg(e))
			return true;
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
		if(!oneArg(e))
			return true;
		CType t;
		if(!typeOf(e->args[0], t)) {
			diag.fail("cannot classify the operand of '" + *e->call.callee + "'");
			return true;
		}
		out = {fn.constInt(i32, typeClassOf(t)), ctInt()};
		return true;
	}

	B32 Emitter::emitFpClassBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& b = *e->call.callee;
		enum Kind : U8 {
			Sign,
			IsInf,
			IsNan,
			IsFinite
		} kind;
		if(detail::fpBuiltinSuffix(b, "signbit"))
			kind = Sign;
		else if(detail::fpBuiltinSuffix(b, "isinf"))
			kind = IsInf;
		else if(detail::fpBuiltinSuffix(b, "isnan"))
			kind = IsNan;
		else if(detail::fpBuiltinSuffix(b, "isfinite"))
			kind = IsFinite;
		else
			return false;
		if(!oneArg(e))
			return true;
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
		Node* pos = fn.constFloat(ty, detail::f64Inf());
		Node* neg = fn.constFloat(ty, -detail::f64Inf());
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

	// __builtin_frame_address and __builtin_return_address
	B32 Emitter::emitFrameBuiltin(Function& fn, const Expr* e, Value& out) {
		const String& name = *e->call.callee;
		if(!oneArg(e))
			return true;
		I64 level = 0;
		if(!evalConst(e->args[0], level)) {
			diag.fail("'" + name + "' expects a constant level");
			return true;
		}
		if(level != 0) {
			out = {fn.constInt(mod.getPtr(), 0), ctVoidPtr()};
			return true;
		}
		out = {fn.call(name, mod.getPtr(), {}, false), ctVoidPtr()};
		return true;
	}

	B32 Emitter::emitPrefetchBuiltin(Function& fn, const Expr* e, Value& out) {
		if(e->args.empty()) {
			diag.fail("'" + *e->call.callee + "' expects at least one argument");
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
		fn.call(*e->call.callee, nullptr, args, false);
		out = voidValue(fn);
		return true;
	}

	B32 Emitter::emitBuiltinCall(Function& fn, const Expr* e, Value& out) {
		if(!e->call.callee)
			return false;
		static const Map<String, BuiltinEmitter> kEmitters = {
				{"__builtin_va_start", &Emitter::emitVaBuiltin},
				{"__builtin_va_end", &Emitter::emitVaBuiltin},
				{"__builtin_va_copy", &Emitter::emitVaCopyBuiltin},
				{"__builtin_alloca", &Emitter::emitAllocaBuiltin},
				{"alloca", &Emitter::emitAllocaBuiltin},
				{"__builtin_setjmp", &Emitter::emitJmpBuiltin},
				{"__builtin_longjmp", &Emitter::emitJmpBuiltin},
				{"__builtin_expect", &Emitter::emitExpectBuiltin},
				{"__builtin_constant_p", &Emitter::emitConstantPBuiltin},
				{"__builtin_unreachable", &Emitter::emitTrapBuiltin},
				{"__builtin_trap", &Emitter::emitTrapBuiltin},
				{"abs", &Emitter::emitAbsBuiltin},
				{"labs", &Emitter::emitAbsBuiltin},
				{"llabs", &Emitter::emitAbsBuiltin},
				{"__builtin_abs", &Emitter::emitAbsBuiltin},
				{"__builtin_labs", &Emitter::emitAbsBuiltin},
				{"__builtin_llabs", &Emitter::emitAbsBuiltin},
				{"__builtin_bswap16", &Emitter::emitBswapBuiltin},
				{"__builtin_bswap32", &Emitter::emitBswapBuiltin},
				{"__builtin_bswap64", &Emitter::emitBswapBuiltin},
				{"__builtin_classify_type", &Emitter::emitClassifyBuiltin},
				{"__builtin_frame_address", &Emitter::emitFrameBuiltin},
				{"__builtin_return_address", &Emitter::emitFrameBuiltin},
				{"__builtin_prefetch", &Emitter::emitPrefetchBuiltin},
		};
		const String& name = *e->call.callee;
		auto found = kEmitters.find(name);
		if(found != kEmitters.end() && (this->*found->second)(fn, e, out))
			return true;
		if(name.rfind("__builtin_", 0) != 0)
			return false;
		if(emitBitCountBuiltin(fn, e, out) || emitOverflowBuiltin(fn, e, out) ||
			 emitFpClassBuiltin(fn, e, out))
			return true;
		if(!detail::isKnownBuiltin(name)) {
			diag.fail("unsupported builtin '" + name + "'");
			return true;
		}
		return false;
	}
} // namespace rat::cc
