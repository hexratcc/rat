#include "emit/emit.h"

namespace rat::cc {
	namespace detail {
		U32 alignedChunkWidth(U32 offset, U32 size) {
			constexpr U32 kMaxChunkBytes = 8;
			U32 w = kMaxChunkBytes;
			while(w > 1 && (offset % w != 0 || offset + w > size))
				w >>= 1;
			return w;
		}
	} // namespace detail

	Emitter::Emitter(Module& module, const TargetLayout& layout)
	: mod(module),
		lay(layout) {}

	U64 Emitter::byteSize(CType t) const { return typeSize(t, lay.ptrBytes); }

	U32 Emitter::alignOf(CType t) const { return typeAlign(t, lay.ptrBytes); }

	CType Emitter::ctSize() const {
		CType t = ctPtrDiff();
		t.set(CType::Unsigned);
		return t;
	}

	CType Emitter::ctPtrDiff() const {
		CType t;
		t.bits = lay.ptrBytes * 8;
		return t;
	}

	Node* Emitter::constSize(Function& fn, U64 value) { return fn.constInt(irType(ctSize()), value); }

	Type* Emitter::byteArrayType(U32 n) { return mod.getArray(mod.getInt(8), n); }

	// whole eightbytes, so register halves of an aggregate can be stored and loaded
	Node* Emitter::allocBytes(Function& fn, U32 size, U32 align) {
		return fn.alloc(byteArrayType((size + 7) & ~7u), align);
	}

	Node* Emitter::declSlot(Function& fn, const Declarator& d, Type* ty, U32 size) {
		if(d.align <= 16)
			return fn.alloc(ty, d.align);
		Node* raw = fn.alloc(byteArrayType(size + d.align - 16), 16);
		Type* sz = irType(ctSize());
		return fn.and_(fn.add(raw, fn.constInt(sz, (I64)d.align - 1)), fn.constInt(sz, -(I64)d.align));
	}

	Node* Emitter::emitArrayElemCount(Function& fn, CType t) {
		Node* total = constSize(fn, 1);
		CType cur = t;
		while(isArrayType(cur)) {
			Node* dim;
			if(cur.array->countExpr) {
				Value v = emitExpr(fn, cur.array->countExpr);
				if(!v.node)
					return nullptr;
				dim = convert(fn, v.node, v.type, ctSize());
			} else {
				dim = constSize(fn, cur.array->count);
			}
			total = fn.mul(total, dim);
			cur = cur.array->elem;
		}
		return total;
	}

	Node* Emitter::emitArrayByteSize(Function& fn, CType t) {
		Node* count = emitArrayElemCount(fn, t);
		if(!count)
			return nullptr;
		CType elem = t;
		while(isArrayType(elem))
			elem = elem.array->elem;
		return fn.mul(count, constSize(fn, byteSize(elem)));
	}

	Type* Emitter::irType(CType t) {
		if(t.ptr > 0 || isArrayType(t) || isAggregate(t))
			return mod.getPtr();
		if(isFloating(t))
			return mod.getFloat(t.bits);
		return mod.getInt(t.bits == 0 ? 32 : t.bits);
	}

	CType Emitter::funcPtrType(const FunctionSignature& sig) {
		FuncType* ft = arena.make<FuncType>();
		ft->ret = sig.returnType;
		for(CType pt : sig.params) {
			Param p;
			p.type = pt;
			ft->params.push_back(p);
		}
		ft->isVarArgs = sig.isVarArgs;
		CType t;
		t.func = ft;
		t.ptr = 1;
		return t;
	}

	Node* Emitter::convert(Function& fn, Node* n, CType from, CType to) {
		if(isComplexType(to)) {
			Value src{n, from};
			return toComplex(fn, src, completeComplex(to)).node;
		}
		if(isComplexType(from)) {
			Value src{n, from};
			Node* re = complexReal(fn, src);
			return convert(fn, re, complexElem(from), to);
		}
		if(isPointer(to) && !isPointer(from) && !isFloating(from)) {
			if(n->getOpcode() == Opcode::Constant)
				return fn.constInt(mod.getPtr(), cast<ConstantNode>(n)->getValue());
			U32 fromBits = from.bits == 0 ? 32 : from.bits;
			Opcode ext = Opcode::SExt;
			if(fromBits < lay.ptrBytes * 8 && from.isUnsigned())
				ext = Opcode::ZExt;
			return fn.convert(ext, n, mod.getPtr());
		}
		if(isPointer(from) || isPointer(to))
			return n;
		B32 fromF = isFloating(from), toF = isFloating(to);
		if(fromF || toF) {
			if(to.isVoid())
				return n;
			if(fromF && toF) { // float <-> float
				if(from.bits == to.bits)
					return n;
				return fn.convert(to.bits > from.bits ? Opcode::FPExt : Opcode::FPTrunc, n, irType(to));
			}
			if(fromF) { // float -> integer
				if(to.bits == 1)
					return fn.compare(Opcode::FNe, n, fn.constFloat(irType(from), 0.0));
				return fn.convert(to.isUnsigned() ? Opcode::FPToUI : Opcode::FPToSI, n, irType(to));
			}
			// integer -> float
			return fn.convert(from.isUnsigned() ? Opcode::UIToFP : Opcode::SIToFP, n, irType(to));
		}
		if(to.isVoid())
			return n;
		if(to.bits == 1)
			return fn.ne(n, fn.constInt(irType(from), 0));
		Node* r = n;
		if(from.bits != to.bits) {
			Opcode resize = Opcode::SExt;
			if(to.bits < from.bits)
				resize = Opcode::Trunc;
			else if(from.isUnsigned())
				resize = Opcode::ZExt;
			r = fn.convert(resize, n, irType(to));
		}
		return reduceBitfield(fn, r, to);
	}

	Node* Emitter::reduceBitfield(Function& fn, Node* n, CType t) {
		if(t.bitPrec == 0 || t.bitPrec >= t.bits || !isInteger(t))
			return n;
		Type* ty = irType(t);
		if(t.isUnsigned())
			return fn.and_(n, fn.constInt(ty, (I64)((1ull << t.bitPrec) - 1)));
		U32 lo = t.bits - t.bitPrec;
		return fn.ashr(fn.shl(n, fn.constInt(ty, lo)), fn.constInt(ty, lo));
	}

	Node* Emitter::toBool(Function& fn, const Value& v) {
		if(isComplexType(v.type)) {
			CType re = complexElem(v.type);
			Node* zero = fn.constFloat(irType(re), 0.0);
			Node* rNz = fn.compare(Opcode::FNe, complexReal(fn, v), zero);
			Node* iNz = fn.compare(Opcode::FNe, complexImag(fn, v), zero);
			return fn.or_(rNz, iNz);
		}
		if(isFloating(v.type))
			return fn.compare(Opcode::FNe, v.node, fn.constFloat(irType(v.type), 0.0));
		return fn.ne(v.node, fn.constInt(irType(v.type), 0));
	}

	Node* Emitter::fromBool(Function& fn, Node* boolean) { return fn.zext(boolean, i32); }

	B32 Emitter::emitMemberLValue(Function& fn, const Expr* e, LValue& out) {
		Node* baseAddr;
		CType structType;
		if(e->member.arrow) {
			Value p = emitExpr(fn, e->member.base);
			if(!p.node)
				return false;
			if(!isPointer(p.type) || !isStruct(pointee(p.type)))
				return diag.fail("'->' requires a pointer to struct or union");
			structType = pointee(p.type);
			baseAddr = p.node;
		} else {
			LValue base;
			if(!emitLValue(fn, e->member.base, base))
				return false;
			if(base.isVar() || !isStruct(base.type))
				return diag.fail("'.' requires a struct or union value");
			structType = base.type;
			baseAddr = base.addr;
		}
		const String& memberName = *e->member.name;
		const Field* f = structType.strukt->find(memberName);
		if(!f)
			return diag.fail("no member named '" + memberName + "' in '" + typeName(structType) + "'");
		out.kind = LValue::Kind::Addr;
		out.addr = f->offset ? fn.add(baseAddr, constSize(fn, f->offset)) : baseAddr;
		out.type = f->type;
		if(isTopVolatile(structType))
			setTopVolatile(out.type);
		out.isArray = f->isArray();
		out.isBitfield = f->isBitfield();
		out.bitWidth = f->bitWidth;
		out.bitOffset = f->bitOffset;
		return true;
	}

	B32 Emitter::emitCompoundLitLValue(Function& fn, const Expr* e, LValue& out) {
		Value v = emitExpr(fn, e);
		if(!v.node)
			return false;
		out.kind = LValue::Kind::Addr;
		if(e->compound.isArray) {
			out.addr = v.node;
			out.type = e->compound.type;
			out.isArray = true;
		} else if(isStruct(e->compound.type)) {
			out.addr = v.node;
			out.type = v.type;
		} else {
			Node* slot = fn.alloc(irType(v.type));
			fn.store(slot, v.node);
			out.addr = slot;
			out.type = v.type;
		}
		return true;
	}

	B32 Emitter::emitLValue(Function& fn, const Expr* e, LValue& out) {
		diag.offset = e->offset;
		if(e->kind == ExprKind::Ident) {
			Local loc;
			if(func.scopes.lookup(*e->ident.name, loc)) {
				if(loc.isArray)
					return diag.fail("array '" + *e->ident.name + "' is not assignable");
				if(loc.inMem()) {
					out.kind = LValue::Kind::Addr;
					out.addr = loc.addr;
				} else {
					out.kind = LValue::Kind::Var;
					out.var = loc.var;
				}
				out.type = loc.type;
				return true;
			}
			auto g = syms.globals.find(*e->ident.name);
			if(g != syms.globals.end()) {
				if(g->second.isArray)
					return diag.fail("array '" + *e->ident.name + "' is not assignable");
				out.kind = LValue::Kind::Addr;
				out.addr = fn.global(syms.resolveAlias(*e->ident.name));
				out.type = g->second.type;
				return true;
			}
			return failUndeclared(*e->ident.name);
		}
		if(e->kind == ExprKind::Unary && e->unary.op == ExprOp::Deref) {
			Value p = emitExpr(fn, e->unary.operand);
			if(!p.node)
				return false;
			if(!isPointer(p.type)) {
				diag.offset = e->offset;
				return diag.fail("indirection requires a pointer operand");
			}
			out.kind = LValue::Kind::Addr;
			out.addr = p.node;
			out.type = pointee(p.type);
			out.isArray = isArrayType(out.type);
			return true;
		}
		if(e->kind == ExprKind::Unary && (e->unary.op == ExprOp::Real || e->unary.op == ExprOp::Imag)) {
			LValue base;
			if(!emitLValue(fn, e->unary.operand, base))
				return false;
			if(!isComplexType(base.type) || base.isVar())
				return diag.fail("'__real__'/'__imag__' require a complex lvalue");
			CType re = complexElem(base.type);
			out.kind = LValue::Kind::Addr;
			out.addr = e->unary.op == ExprOp::Imag ? offsetPtr(fn, base.addr, byteSize(re)) : base.addr;
			out.type = re;
			return true;
		}
		if(e->kind == ExprKind::Member)
			return emitMemberLValue(fn, e, out);
		if(e->kind == ExprKind::CompoundLit)
			return emitCompoundLitLValue(fn, e, out);
		{
			CType st;
			if(typeOf(e, st) && isStruct(st)) {
				Value v = emitExpr(fn, e);
				if(!v.node)
					return false;
				out.kind = LValue::Kind::Addr;
				out.addr = v.node;
				out.type = st;
				return true;
			}
		}
		return diag.fail("expression is not assignable");
	}

	Node* Emitter::loadLValue(Function& fn, const LValue& lv) {
		if(lv.isVar())
			return fn.get(lv.var);
		B32 isVolatile = isTopVolatile(lv.type);
		if(lv.isBitfield) {
			U32 unitBits = byteSize(lv.type) * 8;
			Type* ty = mod.getInt(unitBits);
			Node* unit = fn.load(ty, lv.addr, isVolatile);
			U32 hi = unitBits - lv.bitOffset - lv.bitWidth;
			U32 lo = unitBits - lv.bitWidth;
			Node* n = unit;
			if(hi)
				n = fn.shl(n, fn.constInt(ty, hi));
			Opcode shiftDown = lv.type.isUnsigned() ? Opcode::LShr : Opcode::AShr;
			if(lo)
				n = fn.binary(shiftDown, n, fn.constInt(ty, lo));
			Type* want = irType(lv.type);
			return want == ty ? n : fn.trunc(n, want);
		}
		return fn.load(irType(lv.type), lv.addr, isVolatile);
	}

	void Emitter::storeLValue(Function& fn, const LValue& lv, Node* value) {
		if(lv.isVar()) {
			fn.set(lv.var, value);
			dropRestrictParam(fn, lv.var);
			return;
		}
		B32 isVolatile = isTopVolatile(lv.type);
		if(lv.isBitfield) {
			U32 unitBits = byteSize(lv.type) * 8;
			Type* ty = mod.getInt(unitBits);
			U64 maskBits = lv.bitWidth >= 64 ? ~0ull : ((1ull << lv.bitWidth) - 1);
			U64 shifted = maskBits << lv.bitOffset;
			Node* unit = fn.load(ty, lv.addr, isVolatile);
			Node* cleared = fn.and_(unit, fn.constInt(ty, (I64)~shifted));
			Node* wide = value->getType() == ty ? value : fn.zext(value, ty);
			Node* masked = fn.and_(wide, fn.constInt(ty, (I64)maskBits));
			Node* placed = lv.bitOffset ? fn.shl(masked, fn.constInt(ty, lv.bitOffset)) : masked;
			fn.store(lv.addr, fn.or_(cleared, placed), isVolatile);
			return;
		}
		fn.store(lv.addr, value, isVolatile);
	}

	Node* Emitter::offsetPtr(Function& fn, Node* base, U64 byteOff) {
		return byteOff ? fn.add(base, constSize(fn, byteOff)) : base;
	}

	void Emitter::emitMemCopy(Function& fn, Node* dst, Node* src, U32 size) {
		for(U32 i = 0; i < size;) {
			U32 w = detail::alignedChunkWidth(i, size);
			Type* ty = mod.getInt(w * 8);
			fn.store(offsetPtr(fn, dst, i), fn.load(ty, offsetPtr(fn, src, i)));
			i += w;
		}
	}

	Node* Emitter::elemStride(Function& fn, CType ptrType) {
		CType elem = pointee(ptrType);
		if(isVlaType(elem))
			return emitArrayByteSize(fn, elem);
		return constSize(fn, byteSize(elem));
	}

	B32 Emitter::emit(const TransUnit& unit) {
		i32 = mod.getInt(32);

		for(const FuncDef* def : unit.functions) {
			FunctionSignature sig;
			sig.returnType = def->retType;
			sig.isVarArgs = def->isVarArgs;
			sig.unprototyped = def->unprototyped;
			sig.noInline = def->isNoInline;
			sig.align = def->align;
			for(const Param& p : def->params)
				sig.params.push_back(p.type);
			auto prev = syms.functions.find(def->name);
			if(prev != syms.functions.end()) {
				sig.noInline |= prev->second.noInline;
				if(prev->second.align > sig.align)
					sig.align = prev->second.align; // an earlier declaration may carry it
				if(def->unprototyped && !prev->second.unprototyped) {
					prev->second.noInline = sig.noInline;
					prev->second.align = sig.align;
					continue;
				}
			}
			syms.functions[def->name] = sig;
		}

		if(!registerGlobals(unit))
			return false;

		for(const FuncDef* def : unit.functions) {
			if(!def->aliasOf || mod.getGlobal(def->name))
				continue;
			mod.createAlias(def->name, *def->aliasOf, mod.getInt(8))
					->setLinkage(def->isStatic ? Global::Linkage::Internal : Global::Linkage::External);
		}

		for(const FuncDef* def : unit.functions) {
			if(!def->body)
				continue;
			if(def->isExternInline)
				continue;
			if(!emitFunctionBody(def))
				return false;
		}
		if(!checkAliases())
			return false;
		return !diag.failed;
	}

	B32 Emitter::checkAliases() {
		for(const Global* g : mod.globals()) {
			if(!g->isAlias())
				continue;
			const String& name = g->getName();
			const String& target = g->getAliasTarget();
			const Global* t = mod.getGlobal(target);
			if((t && !t->isAlias()) || mod.getFunction(target))
				continue;
			return diag.fail("alias '" + name + "' names '" + target + "', which is not defined here");
		}
		return true;
	}

	// the ir params of each parameter, an aggregate in registers spreads over its eightbytes
	List<AbiRegs> Emitter::lowerParams(const FuncDef* def, List<Type*>& paramTypes, CallAbi& abi) {
		ArgRegs free = freeArgRegs(def->retType);
		List<AbiRegs> regs;
		for(const Param& p : def->params) {
			AbiRegs r = takeArgRegs(p.type, free);
			regs.push_back(r);
			if(!isAggregate(p.type)) {
				paramTypes.push_back(irType(p.type));
			} else if(!r.count) {
				paramTypes.push_back(mod.getPtr());
				markByval(abi, (U32)paramTypes.size() - 1, p.type);
			}
			for(U32 k = 0; k < r.count; ++k)
				paramTypes.push_back(eightbyteType(r.cls[k]));
		}
		return regs;
	}

	void Emitter::bindFunctionParams(Function& fn,
																	 const FuncDef* def,
																	 U32 paramBase,
																	 const List<AbiRegs>& regs) {
		U32 index = paramBase;
		for(U32 i = 0; i < def->params.size(); ++i) {
			const Param& p = def->params[i];
			if(isAggregate(p.type) && regs[i].count) {
				Node* slot = bindEightbytes(fn, p.type, regs[i], index);
				if(p.name)
					func.scopes.set(*p.name, Local::mem(slot, p.type));
				continue;
			}
			Node* arg = fn.param(index++);
			if(!p.name)
				continue;
			if(isAggregate(p.type)) {
				func.scopes.set(*p.name, Local::mem(arg, p.type));
			} else if(func.addrTaken.count(*p.name) || isTopVolatile(p.type) || dbg.keepLocals) {
				Node* slot = fn.alloc(irType(p.type));
				fn.store(slot, arg, isTopVolatile(p.type));
				func.scopes.set(*p.name, Local::mem(slot, p.type));
			} else {
				Function::Var v = fn.declareLocal(*p.name, arg);
				func.scopes.set(*p.name, Local::inVar(v, p.type));
				if(isPointer(p.type) && isTopRestrict(p.type))
					markRestrictParam(fn, v, index - 1);
			}
		}
	}

	Node* Emitter::bindEightbytes(Function& fn, CType t, const AbiRegs& regs, U32& index) {
		Node* slot = allocBytes(fn, (U32)byteSize(t), alignOf(t));
		for(U32 k = 0; k < regs.count; ++k)
			fn.store(offsetPtr(fn, slot, 8 * k), fn.param(index++));
		return slot;
	}

	void Emitter::markRestrictParam(Function& fn, Function::Var v, U32 index) {
		if(index >= kMaxRestrictParams)
			return;
		fn.getAttrs().restrictParams |= (U64)1 << index;
		func.restrictVars.push_back({v, index});
	}

	void Emitter::dropRestrictParam(Function& fn, Function::Var v) {
		for(const auto& [var, index] : func.restrictVars)
			if(var == v)
				fn.getAttrs().restrictParams &= ~((U64)1 << index);
	}

	B32 Emitter::emitFunctionBody(const FuncDef* def) {
		diag.function = def->name;
		B32 sretReturn = isAggregate(def->retType);
		List<Type*> paramTypes;
		CallAbi abi;
		if(sretReturn) {
			paramTypes.push_back(mod.getPtr());
			abi.ret = returnRegs(def->retType);
		}
		List<AbiRegs> regs = lowerParams(def, paramTypes, abi);
		Type* retTy = nullptr;
		if(sretReturn)
			retTy = mod.getPtr();
		else if(!isVoidType(def->retType))
			retTy = irType(def->retType);
		Function* fn = mod.createFunction(def->name, paramTypes, retTy);
		FunctionAttrs& attrs = fn->getAttrs();
		attrs.variadic = def->isVarArgs;
		attrs.noInline = def->isNoInline || syms.functions[def->name].noInline;
		attrs.align = syms.functions[def->name].align;
		attrs.linkage = def->isStatic ? Linkage::Internal : Linkage::External;
		attrs.abi = std::move(abi);

		func.reset();
		func.returnType = def->retType;
		func.sretSlot = sretReturn ? fn->param(0) : nullptr;
		U32 paramBase = sretReturn ? 1 : 0;
		collectAddrTaken(def->body);
		collectLabels(*fn, def->body);
		func.scopes.push();
		if(stmtHasVla(def->body))
			func.sp = fn->stackSave();
		bindFunctionParams(*fn, def, paramBase, regs);
		debugFunction(*fn, def);
		for(const Param& p : def->params) {
			if(!p.vlaBound)
				continue;
			diag.offset = p.offset;
			if(!emitExpr(*fn, p.vlaBound).node)
				return false;
		}
		if(def->body && !emitStmt(*fn, def->body))
			return false;
		func.scopes.pop();
		for(auto& [name, label] : func.labels)
			fn->seal(label.block);
		if(!fn->blockFinished()) {
			if(func.sretSlot)
				fn->ret(func.sretSlot);
			else if(isVoidType(func.returnType))
				fn->retVoid();
			else
				fn->ret(fn->constInt(irType(func.returnType), 0));
		}
		fn->setLoc(0); // nodes later passes create carry no line
		// statements emitted into unreachable blocks leave anchored nodes with a
		// null control input behind; drop them so every backend sees a graph
		// where the anchor invariant holds even without any opt passes
		fn->pruneUnreachable();
		return true;
	}
} // namespace rat::cc
