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

	static CType ctUnsigned(U32 bits) {
		CType t;
		t.bits = bits;
		t.set(CType::Unsigned);
		return t;
	}

	// __builtin_ names ratcc recognizes but does not implement itself, spelled
	// without the prefix. The first group has a library routine of the same name
	// and is emitted as a plain call to it; the second keeps the __builtin_
	// spelling, so a program that really reaches one gets an honest undefined
	// reference instead of a call to a made-up symbol.
	static const C8* const kLibcBuiltins[] = {
			"abort",		"abs",		 "alloca",	"calloc",	 "copysign", "copysignf", "exit",		 "fabs",
			"fabsf",		"fprintf", "free",		"labs",		 "llabs",		 "longjmp",		"malloc",	 "memcmp",
			"memcpy",		"memmove", "memset",	"printf",	 "putchar",	 "puts",			"realloc", "setjmp",
			"snprintf", "sprintf", "sqrt",		"sqrtf",	 "strcat",	 "strchr",		"strcmp",	 "strcpy",
			"strlen",		"strncat", "strncmp", "strncpy", "strrchr",	 "strstr",
	};
	static const C8* const kUnimplementedBuiltins[] = {
			"add_overflow",	 "add_overflow_p",	"apply",				"apply_args",			"assume_aligned",
			"bswap16",			 "bswap32",					"bswap64",			"classify_type",	"extract_return_addr",
			"frame_address", "isinf",						"isinff",				"isinfl",					"isnan",
			"mul_overflow",	 "mul_overflow_p",	"prefetch",			"return_address", "signbit",
			"signbitf",			 "signbitl",				"sub_overflow", "sub_overflow_p", "trap",
			"va_arg_pack",	 "va_arg_pack_len",
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

	static B32 isKnownBuiltin(const String& name) {
		String stem = name.substr(10);
		return nameIn(kLibcBuiltins, (U32)(sizeof(kLibcBuiltins) / sizeof(kLibcBuiltins[0])), stem) ||
					 nameIn(kUnimplementedBuiltins,
									(U32)(sizeof(kUnimplementedBuiltins) / sizeof(kUnimplementedBuiltins[0])),
									stem);
	}

	// __builtin_clz/ctz/popcount/ffs and their l/ll forms; all return int. clz and
	// ctz are undefined for a zero argument, the same rule gcc gives them, so they
	// map straight onto bsr/bsf.
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
			B32 isConst = evalConst(e->args[0], v);
			out = {fn.constInt(i32, isConst ? 1 : 0), ctInt()};
			return true;
		}
		if(emitBitCountBuiltin(fn, e, out))
			return true;
		if(emitOverflowBuiltin(fn, e, out))
			return true;
		if(b == "__builtin_unreachable") {
			CType vd;
			vd.base = CType::Base::Void;
			out = {fn.constInt(i32, 0), vd};
			return true;
		}
		if(b.rfind("__builtin_", 0) == 0 && !isKnownBuiltin(b)) {
			fail("unsupported builtin '" + b + "'");
			return true;
		}
		return false;
	}

	B32 Emitter::resolveCallee(Function& fn, const Expr* e, Callee& c) {
		if(e->call.callee) {
			auto found = funcs.find(*e->call.callee);
			if(found != funcs.end()) {
				c.direct = true;
				c.sig = found->second;
				c.prototyped = true;
				return true;
			}
			Node* val = nullptr;
			CType ct;
			B32 isObject = false;
			Local loc;
			if(lookup(*e->call.callee, loc) && !loc.isArray) {
				val = loc.inMem() ? fn.load(irType(loc.type), loc.addr) : fn.get(loc.var);
				ct = loc.type;
				isObject = true;
			} else {
				auto g = globalVars.find(*e->call.callee);
				if(g != globalVars.end() && !g->second.isArray) {
					val = fn.load(irType(g->second.type), fn.global(*e->call.callee));
					ct = g->second.type;
					isObject = true;
				}
			}
			if(val && isFuncPtr(ct)) {
				c.target = val;
				c.ft = ct.func;
				c.prototyped = true;
			} else if(isObject) {
				fail("called object is not a function or function pointer");
				return false;
			} else {
				c.direct = true;
				c.sig.ret = ctInt();
			}
			return true;
		}
		Value fpv = emitExpr(fn, e->call.target);
		if(!fpv.node)
			return false;
		if(!isFuncPtr(fpv.type)) {
			fail("called object is not a function");
			return false;
		}
		c.target = fpv.node;
		c.ft = fpv.type.func;
		c.prototyped = true;
		return true;
	}

	B32 Emitter::emitCallArgs(
			Function& fn, const Expr* e, const Callee& c, U32 nparams, List<Node*>& args) {
		for(U32 i = 0; i < e->args.size(); ++i) {
			Value a = emitExpr(fn, e->args[i]);
			if(!a.node)
				return false;
			CType pt;
			if(i < nparams) {
				pt = c.direct ? c.sig.params[i] : c.ft->params[i];
			} else {
				pt = defaultArgPromote(a.type);
				if(isComplexType(pt)) {
					pt.strukt = nullptr;
					pt = completeComplex(pt);
				}
			}
			if(isAggregate(pt)) {
				Node* src = isComplexType(pt) ? toComplex(fn, a, pt).node : a.node;
				Node* tmp = allocBytes(fn, pt.strukt->size);
				emitMemCopy(fn, tmp, src, pt.strukt->size);
				args.push_back(tmp);
			} else {
				args.push_back(convert(fn, a.node, a.type, pt));
			}
		}
		return true;
	}

	Emitter::Value Emitter::emitCall(Function& fn, const Expr* e) {
		Value builtin;
		if(emitBuiltinCall(fn, e, builtin))
			return builtin;

		Callee c;
		if(!resolveCallee(fn, e, c))
			return {};

		CType ret = c.direct ? c.sig.ret : c.ft->ret;
		U32 nparams = c.direct ? (U32)c.sig.params.size() : (U32)c.ft->params.size();
		B32 unproto = c.direct ? c.sig.unprototyped : (c.ft && c.ft->unprototyped);
		if(c.prototyped && !unproto) {
			B32 variadic = c.direct ? c.sig.isVarArgs : c.ft->isVarArgs;
			U32 nargs = (U32)e->args.size();
			if(nargs < nparams || (!variadic && nargs > nparams)) {
				fail(variadic ? "too few arguments to function call"
											: "argument count does not match prototype");
				return {};
			}
		}
		B32 va = true;
		if(c.prototyped && !unproto)
			va = c.direct ? c.sig.isVarArgs : c.ft->isVarArgs;
		String sym;
		if(c.direct) {
			sym = builtinLibcName(*e->call.callee);
			if(sym.empty())
				sym = *e->call.callee;
		}
		List<Node*> args;
		Node* resultSlot = nullptr;
		if(isAggregate(ret)) {
			resultSlot = allocBytes(fn, ret.strukt->size);
			args.push_back(resultSlot);
		}
		if(!emitCallArgs(fn, e, c, nparams, args))
			return {};
		if(resultSlot) {
			if(c.direct)
				fn.call(sym, mod.getPtr(), args, va);
			else
				fn.callIndirect(c.target, mod.getPtr(), args, va);
			return {resultSlot, ret};
		}
		if(c.direct) {
			if(isVoidType(c.sig.ret)) {
				fn.call(sym, nullptr, args, va);
				return {fn.constInt(i32, 0), c.sig.ret};
			}
			return {fn.call(sym, irType(c.sig.ret), args, va), c.sig.ret};
		}
		if(isVoidType(ret)) {
			fn.callIndirect(c.target, nullptr, args, va);
			return {fn.constInt(i32, 0), ret};
		}
		return {fn.callIndirect(c.target, irType(ret), args, va), ret};
	}
} // namespace rat::cc
