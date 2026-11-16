#include "emit/emit.h"

namespace rat::cc {
	B32 Emitter::resolveCallee(Function& fn, const Expr* e, Callee& c) {
		if(e->call.callee) {
			auto found = syms.functions.find(*e->call.callee);
			if(found != syms.functions.end()) {
				c.direct = true;
				c.ft = found->second.type;
				return true;
			}
			Node* val = nullptr;
			CType ct;
			B32 isObject = false;
			Local loc;
			if(func.scopes.lookup(*e->call.callee, loc) && !loc.isArray) {
				if(loc.inMem())
					val = fn.load(irType(loc.type), loc.addr);
				else
					val = fn.get(loc.var);
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
			} else if(isObject) {
				return diag.fail("called object is not a function or function pointer");
			} else {
				c.direct = true;
				c.ft = implicitFuncType(*e->call.callee);
			}
			return true;
		}
		Value fpv = emitExpr(fn, e->call.target);
		if(!fpv.node)
			return false;
		if(!isFuncPtr(fpv.type))
			return diag.fail("called object is not a function");
		c.target = fpv.node;
		c.ft = fpv.type.func;
		return true;
	}

	// one unprototyped type per implicitly declared name, warned about once
	const FuncType* Emitter::implicitFuncType(const String& callee) {
		auto [found, firstUse] = syms.implicitFunctions.try_emplace(callee, nullptr);
		if(!firstUse)
			return found->second;
		if(callee.rfind("__builtin_", 0) != 0)
			diag.warnings.push_back("warning: implicit declaration of function '" + callee + "'");
		FuncType* type = arena.make<FuncType>();
		type->unprototyped = true;
		builtinReturnType(callee, lay.longBits, type->ret);
		found->second = type;
		return type;
	}

	B32 Emitter::emitArgs(Function& fn, const Expr* e, const Callee& c, U32 n, CallArgs& args) {
		for(U32 i = 0; i < e->args.size(); ++i) {
			Value a = emitExpr(fn, e->args[i]);
			if(!a.node)
				return false;
			CType pt;
			if(i < n)
				pt = c.ft->params[i].type;
			else
				pt = completeComplex(defaultArgPromote(a.type));
			if(isAggregate(pt)) {
				Node* src = a.node;
				if(isComplexType(pt))
					src = toComplex(fn, a, pt).node;
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
		Node* tmp = allocBytes(fn, (U32)byteSize(t), alignOf(t));
		emitMemCopy(fn, tmp, src, (U32)byteSize(t));
		AbiRegs regs = takeArgRegs(t, args.free);
		if(!regs.count) {
			args.nodes.push_back(tmp);
			markByval(args.abi, (U32)args.nodes.size() - 1, t);
			return;
		}
		for(U32 k = 0; k < regs.count; ++k) {
			Node* at = offsetPtr(fn, tmp, 8 * k);
			args.nodes.push_back(fn.load(eightbyteType(regs.cls[k]), at));
		}
	}

	// the backend fetches the aggregate by size, alignment and eightbyte classes
	Emitter::Value Emitter::emitVaArgAggregate(Function& fn, Node* ap, CType t) {
		CallAbi fetch;
		fetch.ret = argRegs(t);
		List<Node*> args = {ap, constSize(fn, byteSize(t)), constSize(fn, alignOf(t))};
		Node* src = fn.call("__builtin_va_arg_aggregate", mod.getPtr(), args, true, fetch);
		Node* tmp = allocBytes(fn, (U32)byteSize(t), alignOf(t));
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

		CType ret = c.ft->ret;
		U32 nparams = (U32)c.ft->params.size();
		B32 variadic = c.ft->isVarArgs;
		B32 va = true;
		if(!c.ft->unprototyped) {
			va = variadic;
			U32 nargs = (U32)e->args.size();
			if(nargs < nparams || (!variadic && nargs > nparams)) {
				if(variadic)
					return diag.fail("too few arguments to function call");
				return diag.fail("argument count does not match prototype");
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
			resultSlot = allocBytes(fn, (U32)byteSize(ret), alignOf(ret));
			args.nodes.push_back(resultSlot);
			args.abi.ret = returnRegs(ret);
		}
		if(!emitArgs(fn, e, c, nparams, args))
			return {};
		Type* retTy = isVoidType(ret) ? nullptr : irType(ret);
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
