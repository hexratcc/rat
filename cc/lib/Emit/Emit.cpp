#include "Emit/Emit.h"

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

	U32 Emitter::byteSize(CType t) const { return typeSize(t, lay.ptrBytes); }

	CType Emitter::ctSize() const {
		CType t;
		t.bits = lay.ptrBytes * 8;
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

	Node* Emitter::allocBytes(Function& fn, U32 size) { return fn.alloc(byteArrayType(size)); }

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

	void Emitter::fail(const String& msg) {
		if(failed)
			return;
		errMsg =
				msg + " [@" + std::to_string(curOffset) + (curFunc.empty() ? "" : " in " + curFunc) + "]";
		failed = true;
	}

	void Emitter::pushScope() { scopes.emplace_back(); }

	void Emitter::popScope() { scopes.pop_back(); }

	void Emitter::declare(const String& name, Local local) { scopes.back()[name] = local; }

	B32 Emitter::lookup(const String& name, Local& out) const {
		for(auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
			auto found = it->find(name);
			if(found != it->end()) {
				out = found->second;
				return true;
			}
		}
		return false;
	}

	Type* Emitter::irType(CType t) {
		if(t.ptr > 0)
			return mod.getPtr();
		if(isArrayType(t))
			return mod.getPtr();
		if(isAggregate(t))
			return mod.getPtr();
		if(isFloating(t))
			return mod.getFloat(t.bits);
		return mod.getInt(t.bits == 0 ? 32 : t.bits);
	}

	CType Emitter::funcPtrType(const FnSig& sig) {
		FuncType* ft = arena.make<FuncType>();
		ft->ret = sig.ret;
		ft->params = sig.params;
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
			return fn.convert(Opcode::SExt, n, mod.getPtr());
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
		if(to.isVoid() || from.bits == to.bits)
			return n;
		if(to.bits == 1)
			return fn.ne(n, fn.constInt(irType(from), 0));
		Type* dst = irType(to);
		if(to.bits < from.bits)
			return fn.trunc(n, dst);
		return from.isUnsigned() ? fn.zext(n, dst) : fn.sext(n, dst);
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
			if(!isPointer(p.type) || !isStruct(pointee(p.type))) {
				fail("'->' requires a pointer to struct or union");
				return false;
			}
			structType = pointee(p.type);
			baseAddr = p.node;
		} else {
			LValue base;
			if(!emitLValue(fn, e->member.base, base))
				return false;
			if(base.isVar() || !isStruct(base.type)) {
				fail("'.' requires a struct or union value");
				return false;
			}
			structType = base.type;
			baseAddr = base.addr;
		}
		const Field* f = structType.strukt->find(*e->member.name);
		if(!f) {
			fail("no member named '" + *e->member.name + "' in '" + typeName(structType) + "'");
			return false;
		}
		out.kind = LValue::Kind::Addr;
		out.addr = f->offset ? fn.add(baseAddr, constSize(fn, f->offset)) : baseAddr;
		out.type = f->type;
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
		curOffset = e->offset;
		if(e->kind == ExprKind::Ident) {
			Local loc;
			if(lookup(*e->ident.name, loc)) {
				if(loc.isArray) {
					fail("array '" + *e->ident.name + "' is not assignable");
					return false;
				}
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
			auto g = globalVars.find(*e->ident.name);
			if(g != globalVars.end() && !g->second.isArray) {
				out.kind = LValue::Kind::Addr;
				out.addr = fn.global(*e->ident.name);
				out.type = g->second.type;
				return true;
			}
			auto gv = globalVars.find(*e->ident.name);
			if(gv != globalVars.end() && gv->second.isArray) {
				fail("array '" + *e->ident.name + "' is not assignable");
				return false;
			}
			failUndeclared(*e->ident.name);
			return false;
		}
		if(e->kind == ExprKind::Unary && e->unary.op == ExprOp::Deref) {
			Value p = emitExpr(fn, e->unary.operand);
			if(!p.node)
				return false;
			if(!isPointer(p.type)) {
				curOffset = e->offset;
				fail("indirection requires a pointer operand");
				return false;
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
			if(!isComplexType(base.type) || base.isVar()) {
				fail("'__real__'/'__imag__' require a complex lvalue");
				return false;
			}
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
		fail("expression is not assignable");
		return false;
	}

	Node* Emitter::loadLValue(Function& fn, const LValue& lv) {
		if(lv.isVar())
			return fn.get(lv.var);
		if(lv.isBitfield) {
			Type* ty = irType(lv.type);
			Node* unit = fn.load(ty, lv.addr);
			U32 unitBits = byteSize(lv.type) * 8;
			U32 hi = unitBits - lv.bitOffset - lv.bitWidth;
			U32 lo = unitBits - lv.bitWidth;
			Node* n = unit;
			if(hi)
				n = fn.shl(n, fn.constInt(ty, hi));
			if(lo)
				n = lv.type.isUnsigned() ? fn.lshr(n, fn.constInt(ty, lo)) : fn.ashr(n, fn.constInt(ty, lo));
			return n;
		}
		return fn.load(irType(lv.type), lv.addr);
	}

	void Emitter::storeLValue(Function& fn, const LValue& lv, Node* value) {
		if(lv.isVar()) {
			fn.set(lv.var, value);
			return;
		}
		if(lv.isBitfield) {
			Type* ty = irType(lv.type);
			U64 maskBits = lv.bitWidth >= 64 ? ~0ull : ((1ull << lv.bitWidth) - 1);
			U64 shifted = maskBits << lv.bitOffset;
			Node* unit = fn.load(ty, lv.addr);
			Node* cleared = fn.and_(unit, fn.constInt(ty, (I64)~shifted));
			Node* masked = fn.and_(value, fn.constInt(ty, (I64)maskBits));
			Node* placed = lv.bitOffset ? fn.shl(masked, fn.constInt(ty, lv.bitOffset)) : masked;
			fn.store(lv.addr, fn.or_(cleared, placed));
			return;
		}
		fn.store(lv.addr, value);
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

	Node* Emitter::emitStringLiteral(Function& fn, const Expr* e) {
		return fn.global(internString(e));
	}

	B32 Emitter::emit(const TransUnit& unit) {
		i32 = mod.getInt(32);

		for(const FuncDef* def : unit.functions) {
			FnSig sig;
			sig.ret = def->retType;
			sig.isVarArgs = def->isVarArgs;
			sig.unprototyped = def->unprototyped;
			for(const Param& p : def->params)
				sig.params.push_back(p.type);
			auto prev = funcs.find(def->name);
			if(prev != funcs.end() && def->unprototyped && !prev->second.unprototyped)
				continue;
			funcs[def->name] = sig;
		}

		if(!registerGlobals(unit))
			return false;

		for(const FuncDef* def : unit.functions) {
			if(!def->body)
				continue;
			if(def->isExternInline)
				continue;
			if(!emitFunctionBody(def))
				return false;
		}
		return !failed;
	}

	void Emitter::bindFunctionParams(Function& fn, const FuncDef* def, U32 paramBase) {
		for(U32 i = 0; i < def->params.size(); ++i) {
			const Param& p = def->params[i];
			if(!p.name)
				continue;
			Node* arg = fn.param(paramBase + i);
			if(isAggregate(p.type)) {
				declare(*p.name, Local::mem(arg, p.type));
			} else if(memVars.count(*p.name)) {
				Node* slot = fn.alloc(irType(p.type));
				fn.store(slot, arg);
				declare(*p.name, Local::mem(slot, p.type));
			} else {
				declare(*p.name, Local::inVar(fn.declareLocal(*p.name, arg), p.type));
			}
		}
	}

	B32 Emitter::emitFunctionBody(const FuncDef* def) {
		curFunc = def->name;
		B32 sretReturn = isAggregate(def->retType);
		List<Type*> paramTypes;
		if(sretReturn)
			paramTypes.push_back(mod.getPtr());
		for(const Param& p : def->params)
			paramTypes.push_back(isAggregate(p.type) ? mod.getPtr() : irType(p.type));
		Type* retTy = sretReturn								 ? mod.getPtr()
									: isVoidType(def->retType) ? nullptr
																						 : irType(def->retType);
		Function* fn = mod.createFunction(def->name, paramTypes, retTy);
		fn->setVariadic(def->isVarArgs);
		fn->setLinkage(def->isStatic ? Function::Linkage::Internal : Function::Linkage::External);

		curRet = def->retType;
		sretSlot = sretReturn ? fn->param(0) : nullptr;
		U32 paramBase = sretReturn ? 1 : 0;
		scopes.clear();
		memVars.clear();
		collectAddrTaken(def->body);
		labelBlocks.clear();
		collectLabels(*fn, def->body);
		pushScope();
		bindFunctionParams(*fn, def, paramBase);
		for(const Param& p : def->params) {
			if(!p.vlaBound)
				continue;
			curOffset = p.offset;
			if(!emitExpr(*fn, p.vlaBound).node) {
				popScope();
				return false;
			}
		}
		if(def->body && !emitStmt(*fn, def->body)) {
			popScope();
			return false;
		}
		popScope();
		for(auto& kv : labelBlocks)
			fn->seal(kv.second);
		if(!fn->blockFinished()) {
			if(sretSlot)
				fn->ret(sretSlot);
			else if(isVoidType(curRet))
				fn->retVoid();
			else
				fn->ret(fn->constInt(irType(curRet), 0));
		}
		// statements emitted into unreachable blocks leave anchored nodes with a
		// null control input behind; drop them so every backend sees a graph
		// where the anchor invariant holds even without any opt passes
		fn->pruneUnreachable();
		return true;
	}
} // namespace rat::cc
