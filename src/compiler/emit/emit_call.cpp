#include "emit/emit.h"

namespace rat::cc {
	B32 Emitter::resolveCallee(Function& fn, const Expr* e, Callee& c) {
		if(e->call.callee) {
			auto found = syms.functions.find(*e->call.callee);
			if(found != syms.functions.end()) {
				c.direct = true;
				c.sig = found->second;
				c.prototyped = true;
				return true;
			}
			Node* val = nullptr;
			CType ct;
			B32 isObject = false;
			Local loc;
			if(func.scopes.lookup(*e->call.callee, loc) && !loc.isArray) {
				val = loc.inMem() ? fn.load(irType(loc.type), loc.addr) : fn.get(loc.var);
				ct = loc.type;
				isObject = true;
			} else {
				auto g = syms.globals.find(*e->call.callee);
				if(g != syms.globals.end() && !g->second.isArray) {
					val = fn.load(irType(g->second.type), fn.global(syms.resolveAlias(*e->call.callee)));
					ct = g->second.type;
					isObject = true;
				}
			}
			if(val && isFuncPtr(ct)) {
				c.target = val;
				c.ft = ct.func;
				c.prototyped = true;
			} else if(isObject) {
				diag.fail("called object is not a function or function pointer");
				return false;
			} else {
				const String& callee = *e->call.callee;
				if(callee.rfind("__builtin_", 0) != 0 && syms.implicitFunctions.insert(callee).second)
					diag.warnings.push_back("warning: implicit declaration of function '" + callee + "'");
				c.direct = true;
				if(!builtinReturnType(callee, lay.longBits, c.sig.returnType))
					c.sig.returnType = ctInt();
			}
			return true;
		}
		Value fpv = emitExpr(fn, e->call.target);
		if(!fpv.node)
			return false;
		if(!isFuncPtr(fpv.type)) {
			diag.fail("called object is not a function");
			return false;
		}
		c.target = fpv.node;
		c.ft = fpv.type.func;
		c.prototyped = true;
		return true;
	}

	B32 Emitter::emitArgs(Function& fn, const Expr* e, const Callee& c, U32 n, CallArgs& args) {
		for(U32 i = 0; i < e->args.size(); ++i) {
			Value a = emitExpr(fn, e->args[i]);
			if(!a.node)
				return false;
			CType pt;
			if(i < n)
				pt = c.direct ? c.sig.params[i] : c.ft->params[i].type;
			else
				pt = completeComplex(defaultArgPromote(a.type));
			if(isAggregate(pt)) {
				Node* src = isComplexType(pt) ? toComplex(fn, a, pt).node : a.node;
				pushAggregateArg(fn, src, pt, args);
			} else {
				takeArgRegs(pt, args.free);
				args.nodes.push_back(convert(fn, a.node, a.type, pt));
			}
		}
		return true;
	}

	// a copy whose eightbytes go in registers when they all fit, else on the stack whole
	void Emitter::pushAggregateArg(Function& fn, Node* src, CType t, CallArgs& args) {
		Node* tmp = eightbyteSlot(fn, t);
		emitMemCopy(fn, tmp, src, (U32)byteSize(t));
		AbiRegs regs = takeArgRegs(t, args.free);
		if(!regs.count) {
			args.nodes.push_back(tmp);
			markByval(args.abi, (U32)args.nodes.size() - 1, t);
			return;
		}
		for(U32 k = 0; k < regs.count; ++k)
			if(regs.cls[k] != AbiClass::None)
				args.nodes.push_back(fn.load(eightbyteType(regs.cls[k]), offsetPtr(fn, tmp, 8 * k)));
	}

	// the backend fetches the aggregate by size, alignment and eightbyte classes
	Emitter::Value Emitter::emitVaArgAggregate(Function& fn, Node* ap, CType t) {
		CallAbi fetch;
		fetch.ret = abiRegs(t);
		List<Node*> args = {ap, constSize(fn, byteSize(t)), constSize(fn, alignOf(t))};
		Node* src = fn.call("__builtin_va_arg_aggregate", mod.getPtr(), args, true, fetch);
		Node* tmp = eightbyteSlot(fn, t);
		emitMemCopy(fn, tmp, src, (U32)byteSize(t));
		return {tmp, t};
	}

	Emitter::Value Emitter::emitCall(Function& fn, const Expr* e) {
		Value builtin;
		if(emitBuiltinCall(fn, e, builtin))
			return builtin;

		Callee c;
		if(!resolveCallee(fn, e, c))
			return {};

		CType ret = c.direct ? c.sig.returnType : c.ft->ret;
		U32 nparams = c.direct ? (U32)c.sig.params.size() : (U32)c.ft->params.size();
		B32 unproto = c.direct ? c.sig.unprototyped : c.ft->unprototyped;
		B32 variadic = c.direct ? c.sig.isVarArgs : c.ft->isVarArgs;
		B32 va = true;
		if(c.prototyped && !unproto) {
			va = variadic;
			U32 nargs = (U32)e->args.size();
			if(nargs < nparams || (!variadic && nargs > nparams)) {
				if(variadic)
					diag.fail("too few arguments to function call");
				else
					diag.fail("argument count does not match prototype");
				return {};
			}
		}
		String sym;
		if(c.direct) {
			sym = builtinLibcName(*e->call.callee);
			if(sym.empty())
				sym = *e->call.callee;
		}
		CallArgs args;
		args.free = freeArgRegs(ret);
		Node* resultSlot = nullptr;
		if(isAggregate(ret)) {
			resultSlot = eightbyteSlot(fn, ret);
			args.nodes.push_back(resultSlot);
			args.abi.ret = returnRegs(ret);
		}
		if(!emitArgs(fn, e, c, nparams, args))
			return {};
		Type* retTy = nullptr;
		if(resultSlot)
			retTy = mod.getPtr();
		else if(!isVoidType(ret))
			retTy = irType(ret);
		Node* call;
		if(c.direct)
			call = fn.call(sym, retTy, args.nodes, va, args.abi);
		else
			call = fn.callIndirect(c.target, retTy, args.nodes, va, args.abi);
		if(resultSlot)
			return {resultSlot, ret};
		if(isVoidType(ret))
			return {fn.constInt(i32, 0), ret};
		return {call, ret};
	}
} // namespace rat::cc
