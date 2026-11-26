#include "emit/emit.h"

namespace rat::cc {
	B32 Emitter::resolveCallee(Function& fn, const Expr* e, Callee& c) {
		if(e->call.callee) {
			const String& name = *e->call.callee;
			Symbol sym = lookupSymbol(name);
			if(sym.kind == Symbol::Kind::None) {
				c.direct = true;
				c.ft = implicitFuncType(name);
				return true;
			}
			if(sym.kind == Symbol::Kind::Function) {
				c.direct = true;
				c.ft = sym.type.func;
				return true;
			}
			if(!isFuncPtr(sym.type))
				return diag.fail("called object is not a function or function pointer");
			c.ft = sym.type.func;
			c.target = symbolValue(fn, name, sym).node;
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
			CType pt = a.type;
			if(i < n)
				pt = c.ft->params[i].type;
			else
				pt = defaultArgPromote(pt);
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

	// win64 passes a value not of 1, 2, 4 or 8 bytes by reference
	// sysv va_list is {u32 gp offset, u32 fp offset, ptr overflow area, ptr register save area}
	Emitter::Value Emitter::emitVaArg(Function& fn, Node* ap, CType t) {
		AbiRegs regs = argRegs(t);
		Node* at = nullptr;
		if(lay.win64VaList) {
			at = vaOverflow(fn, ap, 8, 8);
			if(!regs.count)
				at = fn.load(mod.getPtr(), at);
		} else if(regs.count) {
			at = vaRegisters(fn, ap, t, regs);
		} else {
			at = vaOverflow(fn, offsetPtr(fn, ap, 8), (U32)alignUp(byteSize(t), 8), alignOf(t));
		}
		if(!isAggregate(t))
			return {fn.load(irType(t), at), t};
		Node* tmp = allocBytes(fn, (U32)byteSize(t), alignOf(t));
		emitMemCopy(fn, tmp, at, (U32)byteSize(t));
		return {tmp, t};
	}

	Node* Emitter::vaOverflow(Function& fn, Node* overflowAt, U32 step, U32 align) {
		Node* p = fn.load(mod.getPtr(), overflowAt);
		// a stack argument aligns to 16 at most, see X86ArgAssigner::nextMem
		if(align > 8)
			p = fn.and_(fn.add(p, constSize(fn, 15)), constSize(fn, -(U64)16));
		fn.store(overflowAt, fn.add(p, constSize(fn, step)));
		return p;
	}

	// from the register save area when each class has room left, else from the overflow area
	Node* Emitter::vaRegisters(Function& fn, Node* ap, CType t, const AbiRegs& regs) {
		constexpr U32 kSaveAreaEnd[] = {rat::abi::kSysV.gpSaveBytes, rat::abi::kSysV.regSaveBytes};
		constexpr U32 kSaveSlotBytes[] = {8, rat::abi::kSysV.sseSlotBytes};
		Block* stackB = fn.createBlock("va.stack");
		Block* endB = fn.createBlock("va.end");
		Node* offsetAt[2] = {ap, offsetPtr(fn, ap, 4)};
		Node* offset[2] = {};
		for(U32 cls = 0; cls < 2; ++cls) {
			U32 need = regs.countOf(cls ? AbiClass::Sse : AbiClass::Integer);
			if(!need)
				continue;
			offset[cls] = fn.load(i32, offsetAt[cls]);
			Node* last = fn.constInt(i32, kSaveAreaEnd[cls] - kSaveSlotBytes[cls] * need);
			Block* roomB = fn.createBlock("va.room");
			fn.jumpif(fn.compare(Opcode::Ult, last, offset[cls]), stackB);
			fn.jmp(roomB);
			fn.enterBlock(roomB);
		}
		Function::Var at = fn.newVar("va.at", mod.getPtr());
		Node* save = fn.load(mod.getPtr(), offsetPtr(fn, ap, 16));
		// the two eightbytes may sit apart, so they are gathered in a temporary
		Node* tmp = regs.count > 1 ? allocBytes(fn, 16, 8) : nullptr;
		Node* slot = nullptr;
		for(U32 k = 0; k < regs.count; ++k) {
			U32 cls = regs.cls[k] == AbiClass::Sse;
			slot = fn.add(save, fn.zext(offset[cls], mod.getInt(64)));
			offset[cls] = fn.add(offset[cls], fn.constInt(i32, kSaveSlotBytes[cls]));
			if(tmp)
				emitMemCopy(fn, offsetPtr(fn, tmp, 8 * k), slot, 8);
		}
		fn.set(at, tmp ? tmp : slot);
		for(U32 cls = 0; cls < 2; ++cls)
			if(offset[cls])
				fn.store(offsetAt[cls], offset[cls]);
		fn.jmp(endB);
		fn.enterBlock(stackB);
		fn.set(at, vaOverflow(fn, offsetPtr(fn, ap, 8), (U32)alignUp(byteSize(t), 8), alignOf(t)));
		fn.jmp(endB);
		fn.enterBlock(endB);
		return fn.get(at);
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
