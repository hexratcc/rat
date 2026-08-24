#include "emit/emit.h"

namespace rat::cc {
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
				if(!builtinReturnType(*e->call.callee, lay.longBits, c.sig.ret))
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
