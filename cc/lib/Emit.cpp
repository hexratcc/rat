#include "Emit.h"

namespace rat::cc {
	static U32 alignedChunkWidth(U32 offset, U32 size) {
		constexpr U32 kMaxChunkBytes = 8;
		U32 w = kMaxChunkBytes;
		while(w > 1 && (offset % w != 0 || offset + w > size))
			w >>= 1;
		return w;
	}

	Emitter::Emitter(Module& module)
	: mod(module) {}

	U32 Emitter::byteSize(CType t) const { return typeSize(t, mod.pointerBytes()); }

	CType Emitter::ctSize() const { return CType{mod.pointerBytes() * 8, true, false, 0}; }

	CType Emitter::ctPtrDiff() const { return CType{mod.pointerBytes() * 8, false, false, 0}; }

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
			if(to.isVoid)
				return n;
			if(fromF && toF) { // float <-> float
				if(from.bits == to.bits)
					return n;
				return fn.convert(to.bits > from.bits ? Opcode::FPExt : Opcode::FPTrunc, n, irType(to));
			}
			if(fromF) { // float -> integer
				if(to.bits == 1)
					return fn.compare(Opcode::FNe, n, fn.constFloat(irType(from), 0.0));
				return fn.convert(to.isUnsigned ? Opcode::FPToUI : Opcode::FPToSI, n, irType(to));
			}
			// integer -> float
			return fn.convert(from.isUnsigned ? Opcode::UIToFP : Opcode::SIToFP, n, irType(to));
		}
		if(to.isVoid || from.bits == to.bits)
			return n;
		if(to.bits == 1)
			return fn.ne(n, fn.constInt(irType(from), 0));
		Type* dst = irType(to);
		if(to.bits < from.bits)
			return fn.trunc(n, dst);
		return from.isUnsigned ? fn.zext(n, dst) : fn.sext(n, dst);
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
			if(base.isVar || !isStruct(base.type)) {
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
		out.isVar = false;
		out.addr = f->offset ? fn.add(baseAddr, constSize(fn, f->offset)) : baseAddr;
		out.type = f->type;
		out.isArray = f->isArray;
		out.isBitfield = f->isBitfield;
		out.bitWidth = f->bitWidth;
		out.bitOffset = f->bitOffset;
		return true;
	}

	B32 Emitter::emitCompoundLitLValue(Function& fn, const Expr* e, LValue& out) {
		Value v = emitExpr(fn, e);
		if(!v.node)
			return false;
		out.isVar = false;
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
				if(loc.inMem) {
					out.isVar = false;
					out.addr = loc.addr;
				} else {
					out.isVar = true;
					out.var = loc.var;
				}
				out.type = loc.type;
				return true;
			}
			auto g = globals.find(*e->ident.name);
			if(g != globals.end()) {
				out.isVar = false;
				out.addr = fn.global(*e->ident.name);
				out.type = g->second;
				return true;
			}
			if(globalArrays.count(*e->ident.name)) {
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
			out.isVar = false;
			out.addr = p.node;
			out.type = pointee(p.type);
			out.isArray = isArrayType(out.type);
			return true;
		}
		if(e->kind == ExprKind::Unary && (e->unary.op == ExprOp::Real || e->unary.op == ExprOp::Imag)) {
			LValue base;
			if(!emitLValue(fn, e->unary.operand, base))
				return false;
			if(!isComplexType(base.type) || base.isVar) {
				fail("'__real__'/'__imag__' require a complex lvalue");
				return false;
			}
			CType re = complexElem(base.type);
			out.isVar = false;
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
				out.isVar = false;
				out.addr = v.node;
				out.type = st;
				return true;
			}
		}
		fail("expression is not assignable");
		return false;
	}

	Node* Emitter::loadLValue(Function& fn, const LValue& lv) {
		if(lv.isVar)
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
				n = lv.type.isUnsigned ? fn.lshr(n, fn.constInt(ty, lo)) : fn.ashr(n, fn.constInt(ty, lo));
			return n;
		}
		return fn.load(irType(lv.type), lv.addr);
	}

	void Emitter::storeLValue(Function& fn, const LValue& lv, Node* value) {
		if(lv.isVar) {
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
			U32 w = alignedChunkWidth(i, size);
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

	Emitter::Value Emitter::emitPtrArith(Function& fn, ExprOp op, Value lhs, Value rhs) {
		if(op == ExprOp::Add) {
			if(isPointer(lhs.type) && isPointer(rhs.type)) {
				fail("invalid operands to binary '+' (two pointers)");
				return {};
			}
			Value p = isPointer(lhs.type) ? lhs : rhs;
			Value i = isPointer(lhs.type) ? rhs : lhs;
			if(!isInteger(i.type)) {
				fail("pointer arithmetic requires an integer operand");
				return {};
			}
			Node* idx = convert(fn, i.node, i.type, ctSize());
			Node* stride = elemStride(fn, p.type);
			if(!stride)
				return {};
			return {fn.add(p.node, fn.mul(idx, stride)), p.type};
		}
		if(op == ExprOp::Sub) {
			if(isPointer(lhs.type) && isPointer(rhs.type)) {
				CType pd = ctPtrDiff();
				Node* la = fn.convert(Opcode::SExt, lhs.node, irType(pd));
				Node* ra = fn.convert(Opcode::SExt, rhs.node, irType(pd));
				Node* diff = fn.sub(la, ra); // byte difference
				Node* stride = elemStride(fn, lhs.type);
				if(!stride)
					return {};
				return {fn.sdiv(diff, stride), pd};
			}
			if(!isPointer(lhs.type) || !isInteger(rhs.type)) {
				fail("invalid operands to binary '-'");
				return {};
			}
			Node* idx = convert(fn, rhs.node, rhs.type, ctSize());
			Node* stride = elemStride(fn, lhs.type);
			if(!stride)
				return {};
			return {fn.sub(lhs.node, fn.mul(idx, stride)), lhs.type};
		}
		fail("invalid operands to binary expression on a pointer");
		return {};
	}

	Node* Emitter::emitArith(Function& fn, ExprOp op, Node* l, Node* r, CType ct) {
		if(isFloating(ct)) {
			switch(op) {
			case ExprOp::Add:
				return fn.binary(Opcode::FAdd, l, r);
			case ExprOp::Sub:
				return fn.binary(Opcode::FSub, l, r);
			case ExprOp::Mul:
				return fn.binary(Opcode::FMul, l, r);
			case ExprOp::Div:
				return fn.binary(Opcode::FDiv, l, r);
			default:
				fail("invalid operator on a floating-point operand");
				return nullptr;
			}
		}
		switch(op) {
		case ExprOp::Add:
			return fn.add(l, r);
		case ExprOp::Sub:
			return fn.sub(l, r);
		case ExprOp::Mul:
			return fn.mul(l, r);
		case ExprOp::Div:
			return ct.isUnsigned ? fn.udiv(l, r) : fn.sdiv(l, r);
		case ExprOp::Rem:
			return ct.isUnsigned ? fn.urem(l, r) : fn.srem(l, r);
		case ExprOp::Shl:
			return fn.shl(l, r);
		case ExprOp::Shr:
			return ct.isUnsigned ? fn.lshr(l, r) : fn.ashr(l, r);
		case ExprOp::BitAnd:
			return fn.and_(l, r);
		case ExprOp::BitOr:
			return fn.or_(l, r);
		case ExprOp::BitXor:
			return fn.xor_(l, r);
		default:
			fail("unsupported arithmetic operator");
			return nullptr;
		}
	}

	Emitter::Value Emitter::emitIncDec(Function& fn, const Expr* e) {
		LValue lv;
		if(!emitLValue(fn, e->unary.operand, lv))
			return {};
		if(isTopConst(lv.type)) {
			fail("cannot modify a const-qualified lvalue");
			return {};
		}
		CType type = lv.type;
		ExprOp op = e->unary.op;
		B32 isInc = op == ExprOp::PreInc || op == ExprOp::PostInc;
		B32 isPre = op == ExprOp::PreInc || op == ExprOp::PreDec;
		if(isComplexType(type)) {
			CType ct = completeComplex(type);
			Value oldVal = {lv.addr, ct};
			Node* re = complexReal(fn, oldVal);
			Node* im = complexImag(fn, oldVal);
			Node* one = fn.constFloat(irType(complexElem(ct)), 1.0);
			Node* newRe = fn.binary(isInc ? Opcode::FAdd : Opcode::FSub, re, one);
			Value updated = makeComplex(fn, ct, newRe, im);
			storeComplex(fn, lv.addr, ct, updated);
			return isPre ? Value{lv.addr, ct} : makeComplex(fn, ct, re, im);
		}
		Node* old = loadLValue(fn, lv);
		Node* updated;
		if(isFloating(type)) {
			Node* delta = fn.constFloat(irType(type), 1.0);
			updated = fn.binary(isInc ? Opcode::FAdd : Opcode::FSub, old, delta);
		} else {
			Node* delta =
					isPointer(type) ? constSize(fn, byteSize(pointee(type))) : fn.constInt(irType(type), 1);
			updated = isInc ? fn.add(old, delta) : fn.sub(old, delta);
		}
		storeLValue(fn, lv, updated);
		return {isPre ? updated : old, type};
	}

	Emitter::Value
	Emitter::emitStructAssign(Function& fn, const Expr* e, const LValue& lv, Value rhs) {
		CType targetType = lv.type;
		if(e->binary.op != ExprOp::Assign) {
			fail("invalid compound assignment to a struct or union");
			return {};
		}
		if(!isStruct(rhs.type) || rhs.type.strukt != targetType.strukt) {
			fail("assigning to '" + typeName(targetType) + "' from incompatible '" + typeName(rhs.type) +
					 "'");
			return {};
		}
		emitMemCopy(fn, lv.addr, rhs.node, targetType.strukt->size);
		return {lv.addr, targetType};
	}

	Emitter::Value
	Emitter::emitComplexAssign(Function& fn, const Expr* e, const LValue& lv, Value rhs) {
		CType ct = completeComplex(lv.type);
		Value stored;
		if(e->binary.op == ExprOp::Assign) {
			stored = {rhs.node, rhs.type};
		} else {
			ExprOp base;
			B32 ok = compoundBaseOp(e->binary.op, base) && (base == ExprOp::Add || base == ExprOp::Sub ||
																											base == ExprOp::Mul || base == ExprOp::Div);
			if(!ok) {
				fail("invalid compound assignment to a complex operand");
				return {};
			}
			CType opType = completeComplex(usualArithmetic(ct, rhs.type));
			Value cur = toComplex(fn, {lv.addr, ct}, opType);
			Value rc = toComplex(fn, rhs, opType);
			stored = emitComplexBinary(fn, base, cur, rc, opType);
			if(!stored.node)
				return {};
		}
		storeComplex(fn, lv.addr, ct, stored);
		return {lv.addr, ct};
	}

	Emitter::Value Emitter::emitAssign(Function& fn, const Expr* e) {
		LValue lv;
		if(!emitLValue(fn, e->binary.lhs, lv))
			return {};
		if(lv.isArray) {
			fail("array is not assignable");
			return {};
		}
		if(isTopConst(lv.type)) {
			fail("cannot assign to a const-qualified lvalue");
			return {};
		}
		CType targetType = lv.type;
		Value rhs = emitExpr(fn, e->binary.rhs);
		if(!rhs.node)
			return {};

		if(isStruct(targetType))
			return emitStructAssign(fn, e, lv, rhs);

		if(isComplexType(targetType))
			return emitComplexAssign(fn, e, lv, rhs);

		Node* stored = nullptr;
		if(e->binary.op == ExprOp::Assign) {
			stored = convert(fn, rhs.node, rhs.type, targetType);
		} else if(isPointer(targetType) &&
							(e->binary.op == ExprOp::AddAssign || e->binary.op == ExprOp::SubAssign)) {
			ExprOp op = e->binary.op == ExprOp::AddAssign ? ExprOp::Add : ExprOp::Sub;
			Value res = emitPtrArith(fn, op, {loadLValue(fn, lv), targetType}, rhs);
			if(!res.node)
				return {};
			stored = res.node;
		} else {
			ExprOp base;
			if(!compoundBaseOp(e->binary.op, base)) {
				fail("unsupported assignment operator");
				return {};
			}
			CType ct = usualArithmetic(targetType, rhs.type);
			Node* l = convert(fn, loadLValue(fn, lv), targetType, ct);
			Node* r = convert(fn, rhs.node, rhs.type, ct);
			Node* res = emitArith(fn, base, l, r, ct);
			if(!res)
				return {};
			stored = convert(fn, res, ct, targetType);
		}
		storeLValue(fn, lv, stored);
		return {stored, targetType};
	}

	B32 Emitter::emitBuiltinCall(Function& fn, const Expr* e, Value& out) {
		if(!e->call.callee)
			return false;
		const String& b = *e->call.callee;

		if(b == "__builtin_va_start" || b == "__builtin_va_end") {
			List<Node*> args;
			for(const Expr* arg : e->args) {
				Value a = emitExpr(fn, arg);
				if(!a.node)
					return true;
				args.push_back(a.node);
			}
			fn.call(b, nullptr, args);
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

	Emitter::Value Emitter::emitAddrOf(Function& fn, const Expr* e) {
		if(e->unary.operand->kind == ExprKind::Ident) {
			auto f = funcs.find(*e->unary.operand->ident.name);
			if(f != funcs.end())
				return {fn.global(*e->unary.operand->ident.name), funcPtrType(f->second)};
			Local loc;
			if(lookup(*e->unary.operand->ident.name, loc) && loc.isArray)
				return {loc.addr, pointerTo(loc.type)};
			if(globalArrays.count(*e->unary.operand->ident.name))
				return emitExpr(fn, e->unary.operand);
		}
		LValue lv;
		if(!emitLValue(fn, e->unary.operand, lv))
			return {};
		if(lv.isVar) {
			fail("cannot take the address of an SSA value");
			return {};
		}
		return {lv.addr, pointerTo(lv.type)};
	}

	Emitter::Value Emitter::emitDeref(Function& fn, const Expr* e) {
		Value p = emitExpr(fn, e->unary.operand);
		if(!p.node)
			return {};
		if(p.type.func && p.type.ptr == 1)
			return p;
		if(!isPointer(p.type)) {
			curOffset = e->offset;
			fail("indirection requires a pointer operand");
			return {};
		}
		CType pt = pointee(p.type);
		if(isArrayType(pt))
			return {p.node, decay(pt)};
		if(isAggregate(pt))
			return {p.node, pt};
		return {fn.load(irType(pt), p.node), pt};
	}

	Emitter::Value Emitter::emitUnary(Function& fn, const Expr* e) {
		switch(e->unary.op) {
		case ExprOp::PreInc:
		case ExprOp::PreDec:
		case ExprOp::PostInc:
		case ExprOp::PostDec:
			return emitIncDec(fn, e);
		case ExprOp::Addr:
			return emitAddrOf(fn, e);
		case ExprOp::Deref:
			return emitDeref(fn, e);
		default:
			break;
		}
		Value v = emitExpr(fn, e->unary.operand);
		if(!v.node)
			return {};
		if(e->unary.op == ExprOp::Real) {
			if(isComplexType(v.type))
				return {complexReal(fn, v), complexElem(v.type)};
			return v;
		}
		if(e->unary.op == ExprOp::Imag) {
			if(isComplexType(v.type))
				return {complexImag(fn, v), complexElem(v.type)};
			return {fn.constInt(irType(v.type), 0), v.type};
		}
		if(isComplexType(v.type) && (e->unary.op == ExprOp::Pos || e->unary.op == ExprOp::Neg))
			return emitComplexUnary(fn, e->unary.op, v);
		switch(e->unary.op) {
		case ExprOp::Pos: {
			if(!isInteger(v.type) && !isFloating(v.type)) {
				fail("wrong type argument to unary plus");
				return {};
			}
			CType t = promote(v.type);
			return {convert(fn, v.node, v.type, t), t};
		}
		case ExprOp::Neg: {
			if(!isInteger(v.type) && !isFloating(v.type)) {
				fail("wrong type argument to unary minus");
				return {};
			}
			CType t = promote(v.type);
			Node* nv = convert(fn, v.node, v.type, t);
			if(isFloating(t))
				return {fn.unary(Opcode::FNeg, nv), t};
			return {fn.neg(nv), t};
		}
		case ExprOp::BitNot: {
			if(!isInteger(v.type)) {
				fail("wrong type argument to bit-complement");
				return {};
			}
			CType t = promote(v.type);
			return {fn.bitNot(convert(fn, v.node, v.type, t)), t};
		}
		case ExprOp::Not:
			if(isAggregate(v.type) && !isComplexType(v.type)) {
				fail("wrong type argument to unary exclamation mark");
				return {};
			}
			if(isComplexType(v.type))
				return {fromBool(fn, fn.eq(toBool(fn, v), fn.constInt(i32, 1))), ctInt()};
			if(isFloating(v.type))
				return {fromBool(fn, fn.compare(Opcode::FEq, v.node, fn.constFloat(irType(v.type), 0.0))),
								ctInt()};
			return {fromBool(fn, fn.eq(v.node, fn.constInt(irType(v.type), 0))), ctInt()};
		default:
			fail("unsupported unary operator");
			return {};
		}
	}

	Emitter::Value Emitter::emitIdent(Function& fn, const Expr* e) {
		Local loc;
		if(lookup(*e->ident.name, loc)) {
			if(loc.isArray)
				return {loc.addr, pointerTo(loc.type)};
			if(isAggregate(loc.type))
				return {loc.addr, loc.type};
			if(loc.inMem)
				return {fn.load(irType(loc.type), loc.addr), loc.type};
			return {fn.get(loc.var), loc.type};
		}
		auto g = globals.find(*e->ident.name);
		if(g != globals.end()) {
			if(isAggregate(g->second))
				return {fn.global(*e->ident.name), g->second};
			return {fn.load(irType(g->second), fn.global(*e->ident.name)), g->second};
		}
		auto ga = globalArrays.find(*e->ident.name);
		if(ga != globalArrays.end())
			return {fn.global(*e->ident.name), pointerTo(ga->second)};
		auto f = funcs.find(*e->ident.name);
		if(f != funcs.end())
			return {fn.global(*e->ident.name), funcPtrType(f->second)};
		failUndeclared(*e->ident.name);
		return {};
	}

	Emitter::Value Emitter::emitSizeof(Function& fn, const Expr* e) {
		CType sz = ctSize();
		if(e->sizeOf.operand && e->sizeOf.operand->kind == ExprKind::Ident) {
			Local loc;
			if(lookup(*e->sizeOf.operand->ident.name, loc) && loc.lengthNode)
				return {loc.lengthNode, sz};
		}
		if(!e->sizeOf.operand && hasVlaDim(e->sizeOf.type))
			return {emitArrayByteSize(fn, e->sizeOf.type), sz};
		if(e->sizeOf.operand) {
			CType ot;
			if(typeOf(e->sizeOf.operand, ot) && hasVlaDim(ot))
				return {emitArrayByteSize(fn, ot), sz};
		}
		U32 n;
		if(e->sizeOf.operand) {
			if(!sizeofOperand(e->sizeOf.operand, n))
				return {};
		} else {
			CType t = e->sizeOf.type;
			if(isStruct(t) && (t.strukt == nullptr || !t.strukt->complete)) {
				fail("sizeof applied to an incomplete type");
				return {};
			}
			n = byteSize(e->sizeOf.type);
		}
		return {fn.constInt(irType(sz), n), sz};
	}

	Emitter::Value Emitter::emitStmtExpr(Function& fn, const Expr* e) {
		const Stmt* body = e->stmtExpr.body;
		const List<Stmt*>& stmts = body->body;
		pushScope();
		Value result{};
		for(U32 i = 0; i < stmts.size(); ++i) {
			const Stmt* child = stmts[i];
			B32 last = (i + 1 == stmts.size());
			if(last && child->kind == StmtKind::Expr && !fn.blockFinished()) {
				result = emitExpr(fn, child->expr);
				if(!result.node) {
					popScope();
					return {};
				}
				break;
			}
			if(fn.blockFinished() && child->kind != StmtKind::Label && child->kind != StmtKind::Case &&
				 child->kind != StmtKind::Default && !containsLabel(child)) {
				if(child->kind == StmtKind::Decl && !declareDead(fn, child)) {
					popScope();
					return {};
				}
				continue;
			}
			if(!emitStmt(fn, child)) {
				popScope();
				return {};
			}
		}
		popScope();
		if(!result.node)
			return {fn.constInt(mod.getInt(32), 0), ctInt()};
		return result;
	}

	Emitter::Value Emitter::emitExpr(Function& fn, const Expr* e) {
		curOffset = e->offset;
		switch(e->kind) {
		case ExprKind::IntLit: {
			CType t;
			t.isUnsigned = e->intLit.isUnsigned;
			t.bits = e->intLit.isLong ? 64 : 32;
			return {fn.constInt(irType(t), e->intLit.value), t};
		}

		case ExprKind::FloatLit: {
			CType t;
			t.isFloat = true;
			t.bits = e->floatLit.isFloat ? 32 : (e->floatLit.isLongDouble ? 128 : 64);
			Node* lit = fn.constFloat(irType(t), (double)e->floatLit.value);
			if(e->floatLit.isImaginary) {
				CType ct = t;
				ct.isComplex = true;
				return makeComplex(fn, ct, fn.constFloat(irType(t), 0.0), lit);
			}
			return {lit, t};
		}

		case ExprKind::StrLit: {
			CType charPtr;
			charPtr.bits = 8;
			charPtr.ptr = 1;
			return {emitStringLiteral(fn, e), charPtr};
		}

		case ExprKind::Ident:
			return emitIdent(fn, e);

		case ExprKind::Call:
			return emitCall(fn, e);

		case ExprKind::Cast: {
			CType castTy = e->cast.type;
			if(!resolveType(castTy))
				return {};
			Value v = emitExpr(fn, e->cast.operand);
			if(!v.node)
				return {};
			if(!isVoidType(castTy) && !isAggregate(castTy) && isAggregate(v.type) &&
				 !isComplexType(v.type)) {
				fail("cannot cast a struct or union to a scalar type");
				return {};
			}
			if(isVoidType(castTy))
				return {v.node, castTy};
			if(isPointer(v.type) && !isPointer(castTy) && !isFloating(castTy) && !castTy.isVoid) {
				if(castTy.bits == 1)
					return {fn.ne(v.node, fn.constInt(mod.getPtr(), 0)), castTy};
				if(castTy.bits < 64)
					return {fn.trunc(v.node, irType(castTy)), castTy};
			}
			return {convert(fn, v.node, v.type, castTy), castTy};
		}

		case ExprKind::Sizeof:
			return emitSizeof(fn, e);

		case ExprKind::VaArg: {
			Value ap = emitExpr(fn, e->vaArg.ap);
			if(!ap.node)
				return {};
			Type* fetched = isStruct(e->vaArg.type) ? mod.getPtr() : irType(e->vaArg.type);
			Node* r = fn.call("__builtin_va_arg", fetched, {ap.node});
			return {r, e->vaArg.type};
		}

		case ExprKind::StmtExpr:
			return emitStmtExpr(fn, e);

		case ExprKind::Generic: {
			const Expr* sel = genericSelect(e);
			if(!sel)
				return {};
			return emitExpr(fn, sel);
		}

		case ExprKind::Unary:
			return emitUnary(fn, e);

		case ExprKind::Binary:
			return emitBinary(fn, e);

		case ExprKind::Ternary:
			return emitTernary(fn, e);

		case ExprKind::Comma: {
			Value lhs = emitExpr(fn, e->comma.lhs);
			if(!lhs.node)
				return {};
			return emitExpr(fn, e->comma.rhs);
		}

		case ExprKind::Member: {
			LValue lv;
			if(!emitLValue(fn, e, lv))
				return {};
			if(lv.isArray)
				return {lv.addr, pointerTo(lv.type)};
			if(isStruct(lv.type))
				return {lv.addr, lv.type};
			return {loadLValue(fn, lv), lv.type};
		}

		case ExprKind::InitList:
			fail("initializer list is only allowed in a declaration");
			return {};

		case ExprKind::CompoundLit:
			return emitCompoundLit(fn, e);
		}
		fail("unsupported expression");
		return {};
	}

	Emitter::Value Emitter::emitLogicalBinary(Function& fn, const Expr* e) {
		B32 isAnd = e->binary.op == ExprOp::LogAnd;
		Value lhs = emitExpr(fn, e->binary.lhs);
		if(!lhs.node)
			return {};
		if(isAggregate(lhs.type) && !isComplexType(lhs.type)) {
			fail("operand of logical operator must have scalar type");
			return {};
		}
		Node* lpred = toBool(fn, lhs);
		Function::Var var = fn.newVar("logic", i32);
		Function::Block* rhsB = fn.createBlock(isAnd ? "and.rhs" : "or.rhs");
		Function::Block* shortB = fn.createBlock(isAnd ? "and.short" : "or.short");
		Function::Block* endB = fn.createBlock(isAnd ? "and.end" : "or.end");
		fn.jumpif(lpred, isAnd ? rhsB : shortB);
		fn.jmp(isAnd ? shortB : rhsB);
		fn.seal(rhsB);
		fn.setInsertBlock(rhsB);
		Value rhs = emitExpr(fn, e->binary.rhs);
		if(!rhs.node)
			return {};
		if(isAggregate(rhs.type) && !isComplexType(rhs.type)) {
			fail("operand of logical operator must have scalar type");
			return {};
		}
		fn.set(var, fromBool(fn, toBool(fn, rhs)));
		fn.jmp(endB);
		fn.seal(shortB);
		fn.setInsertBlock(shortB);
		fn.set(var, fn.constInt(i32, isAnd ? 0 : 1));
		fn.jmp(endB);
		fn.seal(endB);
		fn.setInsertBlock(endB);
		return {fn.get(var), ctInt()};
	}

	Emitter::Value Emitter::emitComparison(Function& fn, ExprOp op, Value lhs, Value rhs) {
		B32 ptrCmp = isPointer(lhs.type) || isPointer(rhs.type);
		CType ct = ptrCmp ? ctSize() : usualArithmetic(lhs.type, rhs.type);
		Node* l = ptrCmp ? lhs.node : convert(fn, lhs.node, lhs.type, ct);
		Node* r = ptrCmp ? rhs.node : convert(fn, rhs.node, rhs.type, ct);
		Node* cmp = nullptr;
		if(isFloating(ct)) {
			switch(op) {
			case ExprOp::Lt:
				cmp = fn.compare(Opcode::FLt, l, r);
				break;
			case ExprOp::Gt:
				cmp = fn.compare(Opcode::FGt, l, r);
				break;
			case ExprOp::Le:
				cmp = fn.compare(Opcode::FLe, l, r);
				break;
			case ExprOp::Ge:
				cmp = fn.compare(Opcode::FGe, l, r);
				break;
			case ExprOp::Eq:
				cmp = fn.compare(Opcode::FEq, l, r);
				break;
			default:
				cmp = fn.compare(Opcode::FNe, l, r);
				break;
			}
		} else {
			switch(op) {
			case ExprOp::Lt:
				cmp = ct.isUnsigned ? fn.ult(l, r) : fn.slt(l, r);
				break;
			case ExprOp::Gt:
				cmp = ct.isUnsigned ? fn.ugt(l, r) : fn.sgt(l, r);
				break;
			case ExprOp::Le:
				cmp = ct.isUnsigned ? fn.ule(l, r) : fn.sle(l, r);
				break;
			case ExprOp::Ge:
				cmp = ct.isUnsigned ? fn.uge(l, r) : fn.sge(l, r);
				break;
			case ExprOp::Eq:
				cmp = fn.eq(l, r);
				break;
			default:
				cmp = fn.ne(l, r);
				break;
			}
		}
		return {fromBool(fn, cmp), ctInt()};
	}

	Emitter::Value Emitter::emitBinary(Function& fn, const Expr* e) {
		if(isAssignOp(e->binary.op))
			return emitAssign(fn, e);

		if(e->binary.op == ExprOp::LogAnd || e->binary.op == ExprOp::LogOr)
			return emitLogicalBinary(fn, e);

		Value lhs = emitExpr(fn, e->binary.lhs);
		if(!lhs.node)
			return {};
		Value rhs = emitExpr(fn, e->binary.rhs);
		if(!rhs.node)
			return {};

		if(isComplexType(lhs.type) || isComplexType(rhs.type)) {
			ExprOp op = e->binary.op;
			if(op == ExprOp::Eq || op == ExprOp::Ne) {
				CType ct = completeComplex(usualArithmetic(lhs.type, rhs.type));
				Value lc = toComplex(fn, lhs, ct);
				Value rc = toComplex(fn, rhs, ct);
				Node* re = fn.compare(Opcode::FEq, complexReal(fn, lc), complexReal(fn, rc));
				Node* im = fn.compare(Opcode::FEq, complexImag(fn, lc), complexImag(fn, rc));
				Node* eq = fn.and_(re, im);
				Node* res = op == ExprOp::Eq ? eq : fn.eq(eq, fn.constInt(i32, 0));
				return {fromBool(fn, res), ctInt()};
			}
			CType ct = completeComplex(usualArithmetic(lhs.type, rhs.type));
			Value lc = toComplex(fn, lhs, ct);
			Value rc = toComplex(fn, rhs, ct);
			return emitComplexBinary(fn, op, lc, rc, ct);
		}

		switch(e->binary.op) {
		case ExprOp::Rem:
		case ExprOp::Shl:
		case ExprOp::Shr:
		case ExprOp::BitAnd:
		case ExprOp::BitOr:
		case ExprOp::BitXor:
			if(!isInteger(lhs.type) || !isInteger(rhs.type)) {
				fail("operands of this operator must have integer type");
				return {};
			}
			break;
		default:
			break;
		}

		switch(e->binary.op) {
		case ExprOp::Shl:
		case ExprOp::Shr: {
			CType lt = promote(lhs.type);
			CType rt = promote(rhs.type);
			Node* l = convert(fn, lhs.node, lhs.type, lt);
			Node* r = convert(fn, rhs.node, rhs.type, rt);
			return {emitArith(fn, e->binary.op, l, r, lt), lt};
		}
		case ExprOp::Lt:
		case ExprOp::Gt:
		case ExprOp::Le:
		case ExprOp::Ge:
		case ExprOp::Eq:
		case ExprOp::Ne:
			return emitComparison(fn, e->binary.op, lhs, rhs);
		default:
			break;
		}

		if(isPointer(lhs.type) || isPointer(rhs.type))
			return emitPtrArith(fn, e->binary.op, lhs, rhs);

		CType ct = usualArithmetic(lhs.type, rhs.type);
		Node* l = convert(fn, lhs.node, lhs.type, ct);
		Node* r = convert(fn, rhs.node, rhs.type, ct);
		Node* res = emitArith(fn, e->binary.op, l, r, ct);
		if(!res)
			return {};
		return {res, ct};
	}

	Emitter::Value Emitter::emitTernarySelect(Function& fn, const Expr* e) {
		Value cond = emitExpr(fn, e->ternary.cond);
		if(!cond.node)
			return {};
		Value whenTrue = emitExpr(fn, e->ternary.whenTrue);
		if(!whenTrue.node)
			return {};
		Value whenFalse = emitExpr(fn, e->ternary.whenFalse);
		if(!whenFalse.node)
			return {};
		CType ct = usualArithmetic(whenTrue.type, whenFalse.type);
		Node* t = convert(fn, whenTrue.node, whenTrue.type, ct);
		Node* f = convert(fn, whenFalse.node, whenFalse.type, ct);
		if(isFloating(ct)) {
			Node* c = convert(fn, fromBool(fn, toBool(fn, cond)), ctInt(), ct);
			Node* diff = fn.binary(Opcode::FSub, t, f);
			Node* scaled = fn.binary(Opcode::FMul, diff, c);
			return {fn.binary(Opcode::FAdd, f, scaled), ct};
		}
		Node* mask = fn.neg(fromBool(fn, toBool(fn, cond)));
		mask = convert(fn, mask, ctInt(), ct);
		Node* keepTrue = fn.and_(t, mask);
		Node* keepFalse = fn.and_(f, fn.bitNot(mask));
		return {fn.or_(keepTrue, keepFalse), ct};
	}

	Emitter::Value Emitter::emitTernary(Function& fn, const Expr* e) {
		CType tt, ft;
		if(!typeOf(e->ternary.whenTrue, tt) || !typeOf(e->ternary.whenFalse, ft))
			return emitTernarySelect(fn, e);
		B32 isVoidRes = isVoidType(tt) || isVoidType(ft);
		CType rt;
		if(isVoidRes)
			rt = isVoidType(tt) ? tt : ft;
		else if(isComplexType(tt) || isComplexType(ft))
			rt = completeComplex(usualArithmetic(tt, ft));
		else if(isStruct(tt))
			rt = tt;
		else if(isStruct(ft))
			rt = ft;
		else if(isPointer(tt))
			rt = tt;
		else if(isPointer(ft))
			rt = ft;
		else
			rt = usualArithmetic(tt, ft);
		Value cv = emitExpr(fn, e->ternary.cond);
		if(!cv.node)
			return {};
		Node* pred = toBool(fn, cv);
		Function::Var var = 0;
		if(!isVoidRes)
			var = fn.newVar("tern", irType(rt));
		Function::Block* tB = fn.createBlock("tern.true");
		Function::Block* fB = fn.createBlock("tern.false");
		Function::Block* eB = fn.createBlock("tern.end");
		fn.jumpif(pred, tB);
		fn.jmp(fB);
		fn.seal(tB);
		fn.setInsertBlock(tB);
		Value tv = emitExpr(fn, e->ternary.whenTrue);
		if(!tv.node)
			return {};
		if(!fn.blockFinished()) {
			if(!isVoidRes)
				fn.set(var, convert(fn, tv.node, tv.type, rt));
			fn.jmp(eB);
		}
		fn.seal(fB);
		fn.setInsertBlock(fB);
		Value fv = emitExpr(fn, e->ternary.whenFalse);
		if(!fv.node)
			return {};
		if(!fn.blockFinished()) {
			if(!isVoidRes)
				fn.set(var, convert(fn, fv.node, fv.type, rt));
			fn.jmp(eB);
		}
		fn.seal(eB);
		fn.setInsertBlock(eB);
		if(isVoidRes)
			return {fn.constInt(i32, 0), rt};
		return {fn.get(var), rt};
	}

	Emitter::Value Emitter::emitCompoundLit(Function& fn, const Expr* e) {
		CType ty = e->compound.type;
		const Expr* init = e->compound.init;
		if(e->compound.isArray) {
			Type* elemTy = irType(ty);
			U32 elemSize = byteSize(ty);
			I64 count = 0;
			if(e->compound.arrayLen) {
				if(!evalConst(e->compound.arrayLen, count) || count <= 0) {
					failArrayCount();
					return {};
				}
			} else if(init->kind == ExprKind::StrLit) {
				count = (I64)init->str.bytes->size() + 1;
			} else if(init->kind == ExprKind::InitList) {
				I64 cur = 0, maxIdx = -1;
				for(U32 i = 0; i < init->args.size(); ++i) {
					const Designator& des = init->designators[i];
					if(des.isSet) {
						if(!des.isIndex) {
							failFieldInArray();
							return {};
						}
						cur = des.index;
					}
					if(cur > maxIdx)
						maxIdx = cur;
					++cur;
				}
				count = maxIdx + 1;
			}
			if(count <= 0) {
				failArrayCount();
				return {};
			}
			U32 total = (U32)count * elemSize;
			Node* slot =
					isAggregate(ty) ? allocBytes(fn, total) : fn.alloc(mod.getArray(elemTy, (U32)count));
			zeroSlot(fn, slot, total);
			StoreSink sink(*this, fn, slot);
			if(init->kind == ExprKind::StrLit) {
				if(!sink.charArray(0, ty, (U32)count, init))
					return {};
			} else if(!initArrayInit(sink, 0, ty, (U32)count, init))
				return {};
			return {slot, pointerTo(ty)};
		}
		if(isStruct(ty)) {
			Node* slot = allocBytes(fn, ty.strukt->size);
			zeroSlot(fn, slot, ty.strukt->size);
			StoreSink sink(*this, fn, slot);
			if(!initStructInit(sink, 0, ty.strukt, init))
				return {};
			return {slot, ty};
		}
		const Expr* se = init;
		if(se->kind == ExprKind::InitList) {
			if(se->args.empty())
				return {fn.constInt(irType(ty), 0), ty};
			if(se->args.size() != 1 || se->designators[0].isSet) {
				failScalarInit();
				return {};
			}
			se = se->args[0];
		}
		Value v = emitExpr(fn, se);
		if(!v.node)
			return {};
		Node* val = convert(fn, v.node, v.type, ty);
		return {val, ty};
	}

	B32 Emitter::emitIf(Function& fn, const Stmt* s) {
		Value cond = emitExpr(fn, s->expr);
		if(!cond.node)
			return false;
		Node* pred = toBool(fn, cond);

		Function::Block* thenB = fn.createBlock("if.then");
		Function::Block* elseB = s->elseBody ? fn.createBlock("if.else") : nullptr;
		Function::Block* endB = fn.createBlock("if.end");
		B32 reaches = false;

		fn.jumpif(pred, thenB);
		fn.jmp(elseB ? elseB : endB);
		if(!elseB)
			reaches = true;

		fn.seal(thenB);
		fn.setInsertBlock(thenB);
		if(!emitStmt(fn, s->thenBody))
			return false;
		if(!fn.blockFinished()) {
			fn.jmp(endB);
			reaches = true;
		}

		if(elseB) {
			fn.seal(elseB);
			fn.setInsertBlock(elseB);
			if(!emitStmt(fn, s->elseBody))
				return false;
			if(!fn.blockFinished()) {
				fn.jmp(endB);
				reaches = true;
			}
		}

		fn.seal(endB);
		if(reaches)
			fn.setInsertBlock(endB);
		return true;
	}

	B32 Emitter::emitWhile(Function& fn, const Stmt* s) {
		Function::Block* header = fn.createLoopHeader("while.header");
		Function::Block* bodyB = fn.createBlock("while.body");
		Function::Block* exitB = fn.createBlock("while.exit");

		fn.jmp(header);
		fn.setInsertBlock(header);
		Value cond = emitExpr(fn, s->expr);
		if(!cond.node)
			return false;
		fn.jumpif(toBool(fn, cond), bodyB);
		fn.jmp(exitB);

		fn.seal(bodyB);
		fn.setInsertBlock(bodyB);
		loops.push_back({exitB, header, true});
		B32 ok = emitStmt(fn, s->thenBody);
		loops.pop_back();
		if(!ok)
			return false;
		if(!fn.blockFinished())
			fn.jmp(header);

		fn.seal(header);
		fn.seal(exitB);
		fn.setInsertBlock(exitB);
		return true;
	}

	B32 Emitter::emitDoWhile(Function& fn, const Stmt* s) {
		Function::Block* bodyB = fn.createLoopHeader("do.body");
		Function::Block* condB = fn.createBlock("do.cond");
		Function::Block* exitB = fn.createBlock("do.exit");

		fn.jmp(bodyB);
		fn.setInsertBlock(bodyB);
		loops.push_back({exitB, condB, true});
		B32 ok = emitStmt(fn, s->thenBody);
		loops.pop_back();
		if(!ok)
			return false;
		if(!fn.blockFinished())
			fn.jmp(condB);

		fn.seal(condB);
		fn.setInsertBlock(condB);
		Value cond = emitExpr(fn, s->expr);
		if(!cond.node)
			return false;
		fn.jumpif(toBool(fn, cond), bodyB);
		fn.jmp(exitB);

		fn.seal(bodyB);
		fn.seal(exitB);
		fn.setInsertBlock(exitB);
		return true;
	}

	B32 Emitter::emitFor(Function& fn, const Stmt* s) {
		pushScope();
		if(s->forInit && !emitStmt(fn, s->forInit)) {
			popScope();
			return false;
		}

		Function::Block* header = fn.createLoopHeader("for.header");
		Function::Block* bodyB = fn.createBlock("for.body");
		Function::Block* postB = fn.createBlock("for.post");
		Function::Block* exitB = fn.createBlock("for.exit");

		fn.jmp(header);
		fn.setInsertBlock(header);
		B32 exitReachable = false;
		if(s->expr) {
			Value cond = emitExpr(fn, s->expr);
			if(!cond.node) {
				popScope();
				return false;
			}
			fn.jumpif(toBool(fn, cond), bodyB);
			fn.jmp(exitB);
			exitReachable = true;
		} else {
			fn.jmp(bodyB);
		}

		fn.seal(bodyB);
		fn.setInsertBlock(bodyB);
		loops.push_back({exitB, postB, exitReachable});
		B32 ok = emitStmt(fn, s->thenBody);
		LoopFrame frame = loops.back();
		loops.pop_back();
		if(!ok) {
			popScope();
			return false;
		}
		if(!fn.blockFinished())
			fn.jmp(postB);

		fn.seal(postB);
		fn.setInsertBlock(postB);
		if(s->forPost) {
			Value post = emitExpr(fn, s->forPost);
			if(!post.node) {
				popScope();
				return false;
			}
		}
		if(!fn.blockFinished())
			fn.jmp(header);

		fn.seal(header);
		fn.seal(exitB);
		if(frame.exitReachable)
			fn.setInsertBlock(exitB);
		popScope();
		return true;
	}

	B32 Emitter::exprRefersTo(const Expr* e, const String& name) const {
		if(!e)
			return false;
		switch(e->kind) {
		case ExprKind::Ident:
			return e->ident.name && *e->ident.name == name;
		case ExprKind::Unary:
			return exprRefersTo(e->unary.operand, name);
		case ExprKind::Binary:
			return exprRefersTo(e->binary.lhs, name) || exprRefersTo(e->binary.rhs, name);
		case ExprKind::Ternary:
			return exprRefersTo(e->ternary.cond, name) || exprRefersTo(e->ternary.whenTrue, name) ||
						 exprRefersTo(e->ternary.whenFalse, name);
		case ExprKind::Comma:
			return exprRefersTo(e->comma.lhs, name) || exprRefersTo(e->comma.rhs, name);
		case ExprKind::Cast:
			return exprRefersTo(e->cast.operand, name);
		case ExprKind::Sizeof:
			return exprRefersTo(e->sizeOf.operand, name);
		case ExprKind::Member:
			return exprRefersTo(e->member.base, name);
		case ExprKind::VaArg:
			return exprRefersTo(e->vaArg.ap, name);
		case ExprKind::CompoundLit:
			if(exprRefersTo(e->compound.init, name))
				return true;
			break;
		case ExprKind::Call:
			if(exprRefersTo(e->call.target, name))
				return true;
			break;
		default:
			break;
		}
		for(const Expr* a : e->args)
			if(exprRefersTo(a, name))
				return true;
		return false;
	}

	void Emitter::collectSwitchCases(const Stmt* s, List<const Stmt*>& cases, const Stmt*& def) {
		if(!s)
			return;
		switch(s->kind) {
		case StmtKind::Case:
			cases.push_back(s);
			return;
		case StmtKind::Default:
			def = s;
			return;
		case StmtKind::Switch:
			return;
		case StmtKind::Compound:
			for(const Stmt* c : s->body)
				collectSwitchCases(c, cases, def);
			return;
		case StmtKind::If:
			collectSwitchCases(s->thenBody, cases, def);
			collectSwitchCases(s->elseBody, cases, def);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::For:
		case StmtKind::Label:
			collectSwitchCases(s->thenBody, cases, def);
			return;
		default:
			return;
		}
	}

	B32 Emitter::emitSwitch(Function& fn, const Stmt* s) {
		Value ctrl = emitExpr(fn, s->expr);
		if(!ctrl.node)
			return false;
		if(!isInteger(ctrl.type)) {
			fail("switch controlling expression must have integer type");
			return false;
		}
		CType ct = promote(ctrl.type);
		Node* val = convert(fn, ctrl.node, ctrl.type, ct);

		const Stmt* body = s->thenBody;
		if(body->kind != StmtKind::Compound) {
			fail("switch body must be a block");
			return false;
		}

		Function::Block* exitB = fn.createBlock("switch.exit");
		List<const Stmt*> caseStmts;
		const Stmt* defaultStmt = nullptr;
		collectSwitchCases(body, caseStmts, defaultStmt);

		Map<const Stmt*, Function::Block*> blocks;
		List<I64> caseValues;
		List<Function::Block*> caseBlocks;
		for(const Stmt* c : caseStmts) {
			I64 v;
			if(!evalConst(c->expr, v)) {
				fail("case label is not an integer constant expression");
				return false;
			}
			for(I64 prev : caseValues) {
				if(prev == v) {
					fail("duplicate case value in switch");
					return false;
				}
			}
			Function::Block* b = fn.createBlock("switch.case");
			blocks[c] = b;
			caseValues.push_back(v);
			caseBlocks.push_back(b);
		}
		Function::Block* defaultBlock = nullptr;
		if(defaultStmt) {
			defaultBlock = fn.createBlock("switch.default");
			blocks[defaultStmt] = defaultBlock;
		}

		for(U32 i = 0; i < caseValues.size(); ++i) {
			Node* c = fn.eq(val, fn.constInt(irType(ct), caseValues[i]));
			fn.jumpif(c, caseBlocks[i]);
		}
		fn.jmp(defaultBlock ? defaultBlock : exitB);
		switches.push_back(std::move(blocks));
		loops.push_back({exitB, nullptr, false, true});
		B32 ok = emitStmt(fn, body);
		LoopFrame frame = loops.back();
		loops.pop_back();
		switches.pop_back();
		if(!ok)
			return false;

		if(!fn.blockFinished()) {
			fn.jmp(exitB);
			frame.exitReachable = true;
		}
		if(!defaultBlock)
			frame.exitReachable = true;

		fn.seal(exitB);
		if(frame.exitReachable)
			fn.setInsertBlock(exitB);
		return true;
	}

	void Emitter::zeroSlot(Function& fn, Node* slot, U32 size) {
		for(U32 i = 0; i < size;) {
			U32 w = alignedChunkWidth(i, size);
			Type* ty = mod.getInt(w * 8);
			fn.store(offsetPtr(fn, slot, i), fn.constInt(ty, 0));
			i += w;
		}
	}

	B32 Emitter::declareStatic(Function& fn, const Declarator& d) {
		String sym = "__ratcc_static" + std::to_string(staticCounter++) + "_" + *d.name;
		return d.isArray					? registerGlobalArray(d, sym, &fn)
					 : isStruct(d.type) ? registerGlobalStruct(d, sym, &fn)
															: registerGlobalScalar(d, sym, &fn);
	}

	B32 Emitter::declIsVla(const Declarator& d, I64& count) {
		count = 0;
		B32 outerVla = d.arrayLen && !evalConst(d.arrayLen, count);
		return outerVla || isVlaType(d.type);
	}

	B32 Emitter::emitVlaDecl(Function& fn, const Declarator& d) {
		Node* elemBytes = emitArrayByteSize(fn, d.type);
		if(!elemBytes)
			return false;
		Node* bytes = elemBytes;
		if(d.arrayLen) {
			Value v = emitExpr(fn, d.arrayLen);
			if(!v.node)
				return false;
			Node* len = convert(fn, v.node, v.type, ctSize());
			bytes = fn.mul(len, elemBytes);
		}
		Node* slot = fn.allocVLA(irType(d.type), bytes);
		Local loc{0, slot, d.type, true, true, 0};
		loc.lengthNode = bytes;
		declare(*d.name, loc);
		return true;
	}

	B32 Emitter::declareDead(Function& fn, const Stmt* s) {
		for(const Declarator& d : s->decls) {
			if(d.isStatic) {
				if(!declareStatic(fn, d))
					return false;
				continue;
			}
			if(d.isArray) {
				I64 count;
				if(declIsVla(d, count)) {
					if(!emitVlaDecl(fn, d))
						return false;
					continue;
				}
				if(d.arrayLen) {
					if(count <= 0) {
						failArrayCount();
						return false;
					}
				} else if(d.init && d.init->kind == ExprKind::InitList) {
					count = (I64)d.init->args.size();
				} else {
					failArrayUnknownSize(*d.name);
					return false;
				}
				U32 total = (U32)count * byteSize(d.type);
				Node* slot = allocBytes(fn, total);
				declare(*d.name, Local{0, slot, d.type, true, true, (U32)count});
				continue;
			}
			if(isAggregate(d.type)) {
				Node* slot = allocBytes(fn, d.type.strukt->size);
				declare(*d.name, Local{0, slot, d.type, true, false});
				continue;
			}
			if(isArrayType(d.type)) {
				Node* slot = allocBytes(fn, byteSize(d.type));
				declare(*d.name, Local{0, slot, arrayElem(d.type), true, true});
				continue;
			}
			Node* slot = fn.alloc(irType(d.type));
			declare(*d.name, Local{0, slot, d.type, true, false});
		}
		return true;
	}

	B32 Emitter::emitCompound(Function& fn, const Stmt* s) {
		pushScope();
		for(const Stmt* child : s->body) {
			if(fn.blockFinished() && child->kind != StmtKind::Label && child->kind != StmtKind::Case &&
				 child->kind != StmtKind::Default && !containsLabel(child) &&
				 !(!switches.empty() && containsSwitchCase(child))) {
				if(child->kind == StmtKind::Decl && !declareDead(fn, child)) {
					popScope();
					return false;
				}
				continue;
			}
			if(fn.blockFinished() && child->kind != StmtKind::Label && child->kind != StmtKind::Case &&
				 child->kind != StmtKind::Default) {
				Function::Block* dead = fn.createBlock("dead");
				fn.seal(dead);
				fn.setInsertBlock(dead);
			}
			if(!emitStmt(fn, child)) {
				popScope();
				return false;
			}
		}
		popScope();
		return true;
	}

	B32 Emitter::emitCaseLabel(Function& fn, const Stmt* s) {
		if(switches.empty()) {
			fail("'case'/'default' label not within a switch");
			return false;
		}
		auto it = switches.back().find(s);
		if(it == switches.back().end()) {
			fail("'case'/'default' label not within a switch");
			return false;
		}
		Function::Block* lbl = it->second;
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.seal(lbl);
		fn.setInsertBlock(lbl);
		return true;
	}

	B32 Emitter::emitStmt(Function& fn, const Stmt* s) {
		curOffset = s->offset;
		switch(s->kind) {
		case StmtKind::Compound:
			return emitCompound(fn, s);

		case StmtKind::Decl:
			return emitDecl(fn, s);

		case StmtKind::If:
			return emitIf(fn, s);

		case StmtKind::While:
			return emitWhile(fn, s);

		case StmtKind::DoWhile:
			return emitDoWhile(fn, s);

		case StmtKind::For:
			return emitFor(fn, s);

		case StmtKind::Switch:
			return emitSwitch(fn, s);

		case StmtKind::Case:
		case StmtKind::Default:
			return emitCaseLabel(fn, s);

		case StmtKind::Break:
			if(loops.empty()) {
				fail("'break' statement not in a loop or switch");
				return false;
			}
			loops.back().exitReachable = true;
			fn.jmp(loops.back().brk);
			return true;

		case StmtKind::Continue: {
			for(auto it = loops.rbegin(); it != loops.rend(); ++it) {
				if(it->isSwitch)
					continue;
				fn.jmp(it->cont);
				return true;
			}
			fail("'continue' statement not in a loop");
			return false;
		}

		case StmtKind::Return:
			return emitReturn(fn, s);

		case StmtKind::Expr:
			return emitExprStmt(fn, s);

		case StmtKind::Empty:
			return true;

		case StmtKind::Label:
			return emitLabel(fn, s);

		case StmtKind::Goto:
			return emitGoto(fn, s);
		}
		fail("unsupported statement");
		return false;
	}

	B32 Emitter::emitDecl(Function& fn, const Stmt* s) {
		for(const Declarator& d : s->decls)
			if(!emitOneDecl(fn, d))
				return false;
		return true;
	}

	B32 Emitter::emitComplexDecl(Function& fn, const Declarator& d) {
		CType ct = completeComplex(d.type);
		Node* slot = allocBytes(fn, ct.strukt->size);
		if(d.init) {
			if(d.init->kind == ExprKind::InitList) {
				CType re = complexElem(ct);
				zeroSlot(fn, slot, ct.strukt->size);
				const List<Expr*>& els = d.init->args;
				if(!els.empty()) {
					Value rv = emitExpr(fn, els[0]);
					if(!rv.node)
						return false;
					fn.store(slot, convert(fn, rv.node, rv.type, re));
				}
				if(els.size() > 1) {
					Value iv = emitExpr(fn, els[1]);
					if(!iv.node)
						return false;
					fn.store(offsetPtr(fn, slot, byteSize(re)), convert(fn, iv.node, iv.type, re));
				}
			} else {
				Value v = emitExpr(fn, d.init);
				if(!v.node)
					return false;
				storeComplex(fn, slot, ct, v);
			}
		}
		declare(*d.name, Local{0, slot, ct, true, false});
		return true;
	}

	B32 Emitter::emitStructDecl(Function& fn, const Declarator& d) {
		Node* slot = allocBytes(fn, d.type.strukt->size);
		if(d.init && d.init->kind == ExprKind::InitList) {
			zeroSlot(fn, slot, d.type.strukt->size);
			StoreSink sink(*this, fn, slot);
			if(!initStructInit(sink, 0, d.type.strukt, d.init))
				return false;
			declare(*d.name, Local{0, slot, d.type, true, false});
			return true;
		}
		if(d.init) {
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			if(!isStruct(v.type) || v.type.strukt != d.type.strukt) {
				fail("invalid initializer for struct '" + *d.name + "'");
				return false;
			}
			emitMemCopy(fn, slot, v.node, d.type.strukt->size);
		}
		declare(*d.name, Local{0, slot, d.type, true, false});
		return true;
	}

	B32 Emitter::emitTypedefArrayDecl(Function& fn, const Declarator& d) {
		if(d.init) {
			fail("invalid initializer for array variable '" + *d.name + "'");
			return false;
		}
		Node* slot = allocBytes(fn, byteSize(d.type));
		declare(*d.name, Local{0, slot, arrayElem(d.type), true, true});
		return true;
	}

	B32 Emitter::emitMultiDimArrayDecl(Function& fn, const Declarator& d) {
		I64 count;
		if(declIsVla(d, count)) {
			if(d.init) {
				fail("a variable-length array may not be initialized");
				return false;
			}
			return emitVlaDecl(fn, d);
		}
		B32 haveLen = d.arrayLen != nullptr;
		if(haveLen && count <= 0) {
			failArrayCount();
			return false;
		}
		if(!haveLen) {
			if(!d.init || d.init->kind != ExprKind::InitList) {
				failArrayUnknownSize(*d.name);
				return false;
			}
			count = (I64)arrayInitOuterExtent(d.type, d.init);
		}
		U32 elemSize = byteSize(d.type);
		U32 total = (U32)count * elemSize;
		Node* slot = allocBytes(fn, total);
		zeroSlot(fn, slot, total);
		StoreSink sink(*this, fn, slot);
		if(d.init && !initArrayInit(sink, 0, d.type, (U32)count, d.init))
			return false;
		declare(*d.name, Local{0, slot, d.type, true, true, (U32)count});
		return true;
	}

	B32 Emitter::emitArrayDecl(Function& fn, const Declarator& d) {
		if(!d.isArray && isArrayType(d.type))
			return emitTypedefArrayDecl(fn, d);
		if(d.isArray && isArrayType(d.type))
			return emitMultiDimArrayDecl(fn, d);
		B32 haveLen = d.arrayLen != nullptr;
		I64 count;
		if(declIsVla(d, count)) {
			if(d.init) {
				fail("a variable-length array may not be initialized");
				return false;
			}
			return emitVlaDecl(fn, d);
		}
		if(haveLen && count <= 0) {
			failArrayCount();
			return false;
		}
		Type* elemTy = irType(d.type);
		U32 elemSize = byteSize(d.type);

		if(isStruct(d.type)) {
			if(!haveLen) {
				if(!d.init || d.init->kind != ExprKind::InitList) {
					failArrayUnknownSize(*d.name);
					return false;
				}
				count = (I64)arrayInitOuterExtent(d.type, d.init);
			}
			if(count <= 0) {
				failArrayCount();
				return false;
			}
			U32 total = (U32)count * elemSize;
			Node* slot = allocBytes(fn, total);
			zeroSlot(fn, slot, total);
			StoreSink sink(*this, fn, slot);
			if(d.init && !initArrayInit(sink, 0, d.type, (U32)count, d.init))
				return false;
			declare(*d.name, Local{0, slot, d.type, true, true, (U32)count});
			return true;
		}

		if(d.init && d.init->kind == ExprKind::StrLit) {
			B32 wide = d.init->str.isWide;
			U32 cw = wide ? 4u : 1u;
			if(d.type.ptr != 0 || isStruct(d.type) || d.type.bits != cw * 8) {
				failStringNeedsCharArray();
				return false;
			}
			const String& bytes = *d.init->str.bytes;
			I64 nchars = (I64)bytes.size() / (I64)cw;
			if(!haveLen)
				count = nchars + 1;
			Node* slot = fn.alloc(mod.getArray(elemTy, (U32)count));
			for(I64 i = 0; i < count; ++i) {
				I64 val = 0;
				if(i < nchars)
					for(U32 k = 0; k < cw; ++k)
						val |= (I64)(U8)bytes[(U32)(i * cw + k)] << (8 * k);
				fn.store(offsetPtr(fn, slot, (U64)i * elemSize), fn.constInt(elemTy, val));
			}
			declare(*d.name, Local{0, slot, d.type, true, true, (U32)count});
			return true;
		}

		if(d.init && d.init->kind == ExprKind::InitList) {
			const List<Expr*>& els = d.init->args;
			const List<Designator>& des = d.init->designators;
			List<I64> idx(els.size());
			I64 cur = 0, maxIdx = -1;
			for(U32 i = 0; i < els.size(); ++i) {
				if(des[i].isSet) {
					if(!des[i].isIndex) {
						failFieldInArray();
						return false;
					}
					cur = des[i].index;
				}
				idx[i] = cur;
				if(cur > maxIdx)
					maxIdx = cur;
				++cur;
			}
			if(!haveLen)
				count = maxIdx + 1;
			else if(maxIdx >= count) {
				failTooManyInits();
				return false;
			}
			if(count <= 0) {
				failArrayCount();
				return false;
			}
			Node* slot = fn.alloc(mod.getArray(elemTy, (U32)count));
			zeroSlot(fn, slot, (U32)count * elemSize);
			for(U32 i = 0; i < els.size(); ++i) {
				Value v = emitExpr(fn, els[i]);
				if(!v.node)
					return false;
				Node* val = convert(fn, v.node, v.type, d.type);
				fn.store(offsetPtr(fn, slot, (U64)idx[i] * elemSize), val);
			}
			declare(*d.name, Local{0, slot, d.type, true, true, (U32)count});
			return true;
		}

		if(d.init) {
			fail("invalid initializer for an array");
			return false;
		}
		if(!haveLen) {
			failArrayUnknownSize(*d.name);
			return false;
		}
		Node* slot = fn.alloc(mod.getArray(irType(d.type), (U32)count));
		declare(*d.name, Local{0, slot, d.type, true, true, (U32)count});
		return true;
	}

	B32 Emitter::emitOneDecl(Function& fn, const Declarator& d) {
		if(d.isStatic)
			return declareStatic(fn, d);
		if(!d.isArray && isComplexType(d.type))
			return emitComplexDecl(fn, d);
		if(!d.isArray && isStruct(d.type))
			return emitStructDecl(fn, d);
		if(isArrayType(d.type) || d.isArray)
			return emitArrayDecl(fn, d);
		if(d.init && exprRefersTo(d.init, *d.name)) {
			Node* slot = fn.alloc(irType(d.type));
			declare(*d.name, Local{0, slot, d.type, true, false});
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			Node* val = convert(fn, v.node, v.type, d.type);
			fn.store(slot, val);
			return true;
		}
		Node* init;
		if(d.init) {
			Value v = emitExpr(fn, d.init);
			if(!v.node)
				return false;
			init = convert(fn, v.node, v.type, d.type);
		} else {
			init = fn.constInt(irType(d.type), 0);
		}
		if(memVars.count(*d.name)) {
			Node* slot = fn.alloc(irType(d.type));
			fn.store(slot, init);
			declare(*d.name, Local{0, slot, d.type, true, false});
		} else {
			declare(*d.name, Local{fn.declareLocal(*d.name, init), nullptr, d.type, false, false});
		}
		return true;
	}

	B32 Emitter::emitReturn(Function& fn, const Stmt* s) {
		if(sretSlot) {
			if(s->expr) {
				Value v = emitExpr(fn, s->expr);
				if(!v.node)
					return false;
				if(isComplexType(curRet)) {
					storeComplex(fn, sretSlot, completeComplex(curRet), v);
				} else if(!isStruct(v.type) || v.type.strukt != curRet.strukt) {
					fail("invalid return value for a struct/union function");
					return false;
				} else {
					emitMemCopy(fn, sretSlot, v.node, curRet.strukt->size);
				}
			}
			fn.ret(sretSlot);
			return true;
		}
		if(isVoidType(curRet)) {
			if(s->expr) {
				Value v = emitExpr(fn, s->expr);
				if(!v.node)
					return false;
				if(!isVoidType(v.type)) {
					fail("return with a value in a function returning void");
					return false;
				}
			}
			fn.retVoid();
			return true;
		}
		Node* value;
		if(s->expr) {
			Value v = emitExpr(fn, s->expr);
			if(!v.node)
				return false;
			value = convert(fn, v.node, v.type, curRet);
		} else {
			value = fn.constInt(irType(curRet), 0);
		}
		fn.ret(value);
		return true;
	}

	B32 Emitter::emitExprStmt(Function& fn, const Stmt* s) {
		Value v = emitExpr(fn, s->expr);
		return v.node != nullptr;
	}

	B32 Emitter::emitLabel(Function& fn, const Stmt* s) {
		auto it = labelBlocks.find(*s->label);
		if(it == labelBlocks.end()) {
			fail("internal: missing block for label '" + *s->label + "'");
			return false;
		}
		Function::Block* lbl = it->second;
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.setInsertBlock(lbl);
		return emitStmt(fn, s->thenBody);
	}

	B32 Emitter::emitGoto(Function& fn, const Stmt* s) {
		auto it = labelBlocks.find(*s->label);
		if(it == labelBlocks.end()) {
			fail("use of undeclared label '" + *s->label + "'");
			return false;
		}
		fn.jmp(it->second);
		return true;
	}

	void Emitter::collectAddrTakenExpr(const Expr* e) {
		if(!e)
			return;
		switch(e->kind) {
		case ExprKind::Unary:
			if(e->unary.op == ExprOp::Addr && e->unary.operand->kind == ExprKind::Ident)
				memVars.insert(*e->unary.operand->ident.name);
			collectAddrTakenExpr(e->unary.operand);
			return;
		case ExprKind::Binary:
			collectAddrTakenExpr(e->binary.lhs);
			collectAddrTakenExpr(e->binary.rhs);
			return;
		case ExprKind::Ternary:
			collectAddrTakenExpr(e->ternary.cond);
			collectAddrTakenExpr(e->ternary.whenTrue);
			collectAddrTakenExpr(e->ternary.whenFalse);
			return;
		case ExprKind::Comma:
			collectAddrTakenExpr(e->comma.lhs);
			collectAddrTakenExpr(e->comma.rhs);
			return;
		case ExprKind::Cast:
			collectAddrTakenExpr(e->cast.operand);
			return;
		case ExprKind::Call:
			collectAddrTakenExpr(e->call.target);
			for(const Expr* arg : e->args)
				collectAddrTakenExpr(arg);
			return;
		case ExprKind::Member:
			collectAddrTakenExpr(e->member.base);
			return;
		case ExprKind::InitList:
			for(const Expr* el : e->args)
				collectAddrTakenExpr(el);
			return;
		case ExprKind::CompoundLit:
			collectAddrTakenExpr(e->compound.init);
			return;
		case ExprKind::StmtExpr:
			collectAddrTaken(e->stmtExpr.body);
			return;
		default:
			return;
		}
	}

	void Emitter::collectAddrTaken(const Stmt* s) {
		if(!s)
			return;
		switch(s->kind) {
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				collectAddrTaken(child);
			return;
		case StmtKind::Decl:
			for(const Declarator& d : s->decls)
				collectAddrTakenExpr(d.init);
			return;
		case StmtKind::If:
			collectAddrTakenExpr(s->expr);
			collectAddrTaken(s->thenBody);
			collectAddrTaken(s->elseBody);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::Switch:
			collectAddrTakenExpr(s->expr);
			collectAddrTaken(s->thenBody);
			return;
		case StmtKind::For:
			collectAddrTaken(s->forInit);
			collectAddrTakenExpr(s->expr);
			collectAddrTakenExpr(s->forPost);
			collectAddrTaken(s->thenBody);
			return;
		case StmtKind::Label:
			collectAddrTaken(s->thenBody);
			return;
		case StmtKind::Case:
		case StmtKind::Return:
		case StmtKind::Expr:
			collectAddrTakenExpr(s->expr);
			return;
		default:
			return;
		}
	}

	void Emitter::collectLabelsInExpr(Function& fn, const Expr* e) {
		if(!e)
			return;
		switch(e->kind) {
		case ExprKind::StmtExpr:
			collectLabels(fn, e->stmtExpr.body);
			break;
		case ExprKind::Unary:
			collectLabelsInExpr(fn, e->unary.operand);
			break;
		case ExprKind::Binary:
			collectLabelsInExpr(fn, e->binary.lhs);
			collectLabelsInExpr(fn, e->binary.rhs);
			break;
		case ExprKind::Ternary:
			collectLabelsInExpr(fn, e->ternary.cond);
			collectLabelsInExpr(fn, e->ternary.whenTrue);
			collectLabelsInExpr(fn, e->ternary.whenFalse);
			break;
		case ExprKind::Comma:
			collectLabelsInExpr(fn, e->comma.lhs);
			collectLabelsInExpr(fn, e->comma.rhs);
			break;
		case ExprKind::Cast:
			collectLabelsInExpr(fn, e->cast.operand);
			break;
		case ExprKind::Sizeof:
			collectLabelsInExpr(fn, e->sizeOf.operand);
			break;
		case ExprKind::Member:
			collectLabelsInExpr(fn, e->member.base);
			break;
		case ExprKind::Call:
			collectLabelsInExpr(fn, e->call.target);
			break;
		case ExprKind::VaArg:
			collectLabelsInExpr(fn, e->vaArg.ap);
			break;
		default:
			break;
		}
		for(const Expr* a : e->args)
			collectLabelsInExpr(fn, a);
	}

	void Emitter::collectLabels(Function& fn, const Stmt* s) {
		if(!s)
			return;
		switch(s->kind) {
		case StmtKind::Label:
			if(!labelBlocks.count(*s->label))
				labelBlocks[*s->label] = fn.createLoopHeader("label." + *s->label);
			collectLabels(fn, s->thenBody);
			return;
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				collectLabels(fn, child);
			return;
		case StmtKind::If:
			collectLabelsInExpr(fn, s->expr);
			collectLabels(fn, s->thenBody);
			collectLabels(fn, s->elseBody);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::Switch:
			collectLabelsInExpr(fn, s->expr);
			collectLabels(fn, s->thenBody);
			return;
		case StmtKind::For:
			collectLabels(fn, s->forInit);
			collectLabelsInExpr(fn, s->expr);
			collectLabelsInExpr(fn, s->forPost);
			collectLabels(fn, s->thenBody);
			return;
		case StmtKind::Expr:
		case StmtKind::Return:
		case StmtKind::Case:
			collectLabelsInExpr(fn, s->expr);
			return;
		case StmtKind::Decl:
			for(const Declarator& d : s->decls)
				collectLabelsInExpr(fn, d.init);
			return;
		default:
			return;
		}
	}

	B32 Emitter::containsLabelInExpr(const Expr* e) {
		if(!e)
			return false;
		switch(e->kind) {
		case ExprKind::StmtExpr:
			if(containsLabel(e->stmtExpr.body))
				return true;
			break;
		case ExprKind::Unary:
			if(containsLabelInExpr(e->unary.operand))
				return true;
			break;
		case ExprKind::Binary:
			if(containsLabelInExpr(e->binary.lhs) || containsLabelInExpr(e->binary.rhs))
				return true;
			break;
		case ExprKind::Ternary:
			if(containsLabelInExpr(e->ternary.cond) || containsLabelInExpr(e->ternary.whenTrue) ||
				 containsLabelInExpr(e->ternary.whenFalse))
				return true;
			break;
		case ExprKind::Comma:
			if(containsLabelInExpr(e->comma.lhs) || containsLabelInExpr(e->comma.rhs))
				return true;
			break;
		case ExprKind::Cast:
			if(containsLabelInExpr(e->cast.operand))
				return true;
			break;
		case ExprKind::Sizeof:
			if(containsLabelInExpr(e->sizeOf.operand))
				return true;
			break;
		case ExprKind::Member:
			if(containsLabelInExpr(e->member.base))
				return true;
			break;
		case ExprKind::Call:
			if(containsLabelInExpr(e->call.target))
				return true;
			break;
		case ExprKind::VaArg:
			if(containsLabelInExpr(e->vaArg.ap))
				return true;
			break;
		default:
			break;
		}
		for(const Expr* a : e->args)
			if(containsLabelInExpr(a))
				return true;
		return false;
	}

	B32 Emitter::containsLabel(const Stmt* s) {
		if(!s)
			return false;
		switch(s->kind) {
		case StmtKind::Label:
			return true;
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				if(containsLabel(child))
					return true;
			return false;
		case StmtKind::If:
			return containsLabelInExpr(s->expr) || containsLabel(s->thenBody) ||
						 containsLabel(s->elseBody);
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::Switch:
			return containsLabelInExpr(s->expr) || containsLabel(s->thenBody);
		case StmtKind::For:
			return containsLabel(s->forInit) || containsLabelInExpr(s->expr) ||
						 containsLabelInExpr(s->forPost) || containsLabel(s->thenBody);
		case StmtKind::Expr:
		case StmtKind::Return:
		case StmtKind::Case:
			return containsLabelInExpr(s->expr);
		case StmtKind::Decl:
			for(const Declarator& d : s->decls)
				if(containsLabelInExpr(d.init))
					return true;
			return false;
		default:
			return false;
		}
	}

	B32 Emitter::containsSwitchCase(const Stmt* s) {
		if(!s)
			return false;
		switch(s->kind) {
		case StmtKind::Case:
		case StmtKind::Default:
			return true;
		case StmtKind::Label:
			return containsSwitchCase(s->thenBody);
		case StmtKind::Compound:
			for(const Stmt* child : s->body)
				if(containsSwitchCase(child))
					return true;
			return false;
		case StmtKind::If:
			return containsSwitchCase(s->thenBody) || containsSwitchCase(s->elseBody);
		case StmtKind::While:
		case StmtKind::DoWhile:
			return containsSwitchCase(s->thenBody);
		case StmtKind::For:
			return containsSwitchCase(s->thenBody);
		default:
			return false;
		}
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
				declare(*p.name, Local{0, arg, p.type, true, false});
			} else if(memVars.count(*p.name)) {
				Node* slot = fn.alloc(irType(p.type));
				fn.store(slot, arg);
				declare(*p.name, Local{0, slot, p.type, true, false});
			} else {
				declare(*p.name, Local{fn.declareLocal(*p.name, arg), nullptr, p.type, false, false});
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
		return true;
	}
} // namespace rat::cc
