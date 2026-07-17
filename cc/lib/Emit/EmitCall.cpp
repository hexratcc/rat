#include "Emit/Emit.h"

namespace rat::cc {
	Node* Emitter::vaListRef(Function& fn, const Expr* ap) {
		if(!vaPtr)
			return emitExpr(fn, ap).node;
		LValue lv;
		if(!emitLValue(fn, ap, lv))
			return nullptr;
		if(lv.isVar || !lv.addr) {
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
			v.isVoid = true;
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
			if(vaPtr) {
				Value src = emitExpr(fn, e->args[1]);
				if(!src.node)
					return true;
				fn.store(dst, src.node);
			} else {
				Value src = emitExpr(fn, e->args[1]);
				if(!src.node)
					return true;
				emitMemCopy(fn, dst, src.node, ptrBytes * 3);
			}
			CType v;
			v.isVoid = true;
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
		if(b == "__builtin_unreachable") {
			CType vd;
			vd.isVoid = true;
			out = {fn.constInt(i32, 0), vd};
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
				val = loc.inMem ? fn.load(irType(loc.type), loc.addr) : fn.get(loc.var);
				ct = loc.type;
				isObject = true;
			} else {
				auto g = globals.find(*e->call.callee);
				if(g != globals.end()) {
					val = fn.load(irType(g->second), fn.global(*e->call.callee));
					ct = g->second;
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
				fn.call(*e->call.callee, mod.getPtr(), args);
			else
				fn.callIndirect(c.target, mod.getPtr(), args);
			return {resultSlot, ret};
		}
		if(c.direct) {
			if(isVoidType(c.sig.ret)) {
				fn.call(*e->call.callee, nullptr, args);
				return {fn.constInt(i32, 0), c.sig.ret};
			}
			return {fn.call(*e->call.callee, irType(c.sig.ret), args), c.sig.ret};
		}
		if(isVoidType(ret)) {
			fn.callIndirect(c.target, nullptr, args);
			return {fn.constInt(i32, 0), ret};
		}
		return {fn.callIndirect(c.target, irType(ret), args), ret};
	}
} // namespace rat::cc
