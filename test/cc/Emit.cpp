#include "Emit.h"

#include "IR/Function.h"
#include "IR/Module.h"

namespace rat::cc {
	Emitter::Emitter(Module& module) : mod(module) {}

	U32 Emitter::byteSize(CType t) const {
		return typeSize(t, mod.pointerBytes());
	}

	CType Emitter::ctSize() const {
		return CType{mod.pointerBytes() * 8, true, false, 0};
	}

	CType Emitter::ctPtrDiff() const {
		return CType{mod.pointerBytes() * 8, false, false, 0};
	}

	Node* Emitter::constSize(Function& fn, U64 value) {
		return fn.constInt(irType(ctSize()), value);
	}

	Node* Emitter::allocBytes(Function& fn, U32 size) {
		return fn.alloc(mod.getArray(mod.getInt(8), size));
	}

	Node* Emitter::emitArrayElemCount(Function& fn, CType t) {
		Node* total = constSize(fn, 1);
		CType cur = t;
		while (isArrayType(cur)) {
			Node* dim;
			if (cur.array->countExpr) {
				Value v = emitExpr(fn, cur.array->countExpr);
				if (!v.node)
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
		if (!count)
			return nullptr;
		CType elem = t;
		while (isArrayType(elem))
			elem = elem.array->elem;
		return fn.mul(count, constSize(fn, byteSize(elem)));
	}

	void Emitter::fail(const String& msg) {

	void Emitter::fail(const String& msg) {
		if (failed)
			return;
		errMsg = msg + " [@" + std::to_string(curOffset) +
						 (curFunc.empty() ? "" : " in " + curFunc) + "]";
		failed = true;
	}

	void Emitter::pushScope() { scopes.emplace_back(); }

	void Emitter::popScope() { scopes.pop_back(); }

	void Emitter::declare(const String& name, Local local) {
		scopes.back()[name] = local;
		scopes.back()[name] = var;
	}

	B32 Emitter::lookup(const String& name, Local& out) const {
	B32 Emitter::lookup(const String& name, U32& var) const {
		for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
			auto found = it->find(name);
			if (found != it->end()) {
				out = found->second;
				var = found->second;
				return true;
			}
		}
		return false;
	}

	Type* Emitter::irType(CType t) {
		if (t.ptr > 0)
			return mod.getPtr();
		if (isArrayType(t))
			return mod.getPtr();
		if (isAggregate(t))
			return mod.getPtr();
		if (isFloating(t))
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
		if (isComplexType(to)) {
			Value src{n, from};
			return toComplex(fn, src, completeComplex(to)).node;
		}
		if (isComplexType(from)) {
			Value src{n, from};
			Node* re = complexReal(fn, src);
			return convert(fn, re, complexElem(from), to);
		}
		if (isPointer(to) && !isPointer(from) && !isFloating(from)) {
			if (n->getOpcode() == Opcode::Constant)
				return fn.constInt(mod.getPtr(), cast<ConstantNode>(n)->getValue());
			return fn.convert(Opcode::SExt, n, mod.getPtr());
		}
		if (isPointer(from) || isPointer(to))
			return n;
		B32 fromF = isFloating(from), toF = isFloating(to);
		if (fromF || toF) {
			if (to.isVoid)
				return n;
			if (fromF && toF) { // float <-> float
				if (from.bits == to.bits)
					return n;
				return fn.convert(to.bits > from.bits ? Opcode::FPExt : Opcode::FPTrunc,
													n, irType(to));
			}
			if (fromF) { // float -> integer
				if (to.bits == 1)
					return fn.compare(Opcode::FNe, n, fn.constFloat(irType(from), 0.0));
				return fn.convert(to.isUnsigned ? Opcode::FPToUI : Opcode::FPToSI, n,
													irType(to));
			}
			// integer -> float
			return fn.convert(from.isUnsigned ? Opcode::UIToFP : Opcode::SIToFP, n,
												irType(to));
		}
		if (to.isVoid || from.bits == to.bits)
			return n;
		if (to.bits == 1)
			return fn.ne(n, fn.constInt(irType(from), 0));
		Type* dst = irType(to);
		if (to.bits < from.bits)
			return fn.trunc(n, dst);
		return from.isUnsigned ? fn.zext(n, dst) : fn.sext(n, dst);
	}

	Node* Emitter::toBool(Function& fn, const Value& v) {
		if (isComplexType(v.type)) {
			CType re = complexElem(v.type);
			Node* zero = fn.constFloat(irType(re), 0.0);
			Node* rNz = fn.compare(Opcode::FNe, complexReal(fn, v), zero);
			Node* iNz = fn.compare(Opcode::FNe, complexImag(fn, v), zero);
			return fn.or_(rNz, iNz);
		}
		if (isFloating(v.type))
			return fn.compare(Opcode::FNe, v.node,
												fn.constFloat(irType(v.type), 0.0));
		return fn.ne(v.node, fn.constInt(irType(v.type), 0));
	}

	Node* Emitter::fromBool(Function& fn, Node* boolean) {
		return fn.zext(boolean, i32);
	}

	B32 Emitter::emitMemberLValue(Function& fn, const Expr* e, LValue& out) {
		Node* baseAddr;
		CType structType;
		if (e->member.arrow) {
			Value p = emitExpr(fn, e->member.base);
			if (!p.node)
				return false;
			if (!isPointer(p.type) || !isStruct(pointee(p.type))) {
				fail("'->' requires a pointer to struct or union");
				return false;
			}
			structType = pointee(p.type);
			baseAddr = p.node;
		} else {
			LValue base;
			if (!emitLValue(fn, e->member.base, base))
				return false;
			if (base.isVar || !isStruct(base.type)) {
				fail("'.' requires a struct or union value");
				return false;
			}
			structType = base.type;
			baseAddr = base.addr;
		}
		const Field* f = structType.strukt->find(*e->member.name);
		if (!f) {
			fail("no member named '" + *e->member.name + "' in '" +
					 typeName(structType) + "'");
			return false;
		}
		out.isVar = false;
		out.addr =
				f->offset ? fn.add(baseAddr, constSize(fn, f->offset)) : baseAddr;
		out.type = f->type;
		out.isArray = f->isArray;
		out.isBitfield = f->isBitfield;
		out.bitWidth = f->bitWidth;
		out.bitOffset = f->bitOffset;
		return true;
	}

	B32 Emitter::emitCompoundLitLValue(Function& fn, const Expr* e, LValue& out) {
		Value v = emitExpr(fn, e);
		if (!v.node)
			return false;
		out.isVar = false;
		if (e->compound.isArray) {
			out.addr = v.node;
			out.type = e->compound.type;
			out.isArray = true;
		} else if (isStruct(e->compound.type)) {
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
		if (e->kind == ExprKind::Ident) {
			Local loc;
			if (lookup(*e->ident.name, loc)) {
				if (loc.isArray) {
					fail("array '" + *e->ident.name + "' is not assignable");
					return false;
				}
				if (loc.inMem) {
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
			if (g != globals.end()) {
				out.isVar = false;
				out.addr = fn.global(*e->ident.name);
				out.type = g->second;
				return true;
			}
			if (globalArrays.count(*e->ident.name)) {
				fail("array '" + *e->ident.name + "' is not assignable");
				return false;
			}
			failUndeclared(*e->ident.name);
			return false;
		}
		if (e->kind == ExprKind::Unary && e->unary.op == ExprOp::Deref) {
			Value p = emitExpr(fn, e->unary.operand);
			if (!p.node)
				return false;
			if (!isPointer(p.type)) {
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
		if (e->kind == ExprKind::Unary &&
				(e->unary.op == ExprOp::Real || e->unary.op == ExprOp::Imag)) {
			LValue base;
			if (!emitLValue(fn, e->unary.operand, base))
				return false;
			if (!isComplexType(base.type) || base.isVar) {
				fail("'__real__'/'__imag__' require a complex lvalue");
				return false;
			}
			CType re = complexElem(base.type);
			out.isVar = false;
			out.addr = e->unary.op == ExprOp::Imag
										 ? offsetPtr(fn, base.addr, byteSize(re))
										 : base.addr;
			out.type = re;
			return true;
		}
		if (e->kind == ExprKind::Member)
			return emitMemberLValue(fn, e, out);
		if (e->kind == ExprKind::CompoundLit)
			return emitCompoundLitLValue(fn, e, out);
		{
			CType st;
			if (typeOf(e, st) && isStruct(st)) {
				Value v = emitExpr(fn, e);
				if (!v.node)
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
		if (lv.isVar)
			return fn.get(lv.var);
		if (lv.isBitfield) {
			Type* ty = irType(lv.type);
			Node* unit = fn.load(ty, lv.addr);
			U32 unitBits = byteSize(lv.type) * 8;
			U32 hi = unitBits - lv.bitOffset - lv.bitWidth;
			U32 lo = unitBits - lv.bitWidth;
			Node* n = unit;
			if (hi)
				n = fn.shl(n, fn.constInt(ty, hi));
			if (lo)
				n = lv.type.isUnsigned ? fn.lshr(n, fn.constInt(ty, lo))
															 : fn.ashr(n, fn.constInt(ty, lo));
			return n;
		}
		return fn.load(irType(lv.type), lv.addr);
	}

	void Emitter::storeLValue(Function& fn, const LValue& lv, Node* value) {
		if (lv.isVar) {
			fn.set(lv.var, value);
			return;
		}
		if (lv.isBitfield) {
			Type* ty = irType(lv.type);
			U64 maskBits = lv.bitWidth >= 64 ? ~0ull : ((1ull << lv.bitWidth) - 1);
			U64 shifted = maskBits << lv.bitOffset;
			Node* unit = fn.load(ty, lv.addr);
			Node* cleared = fn.and_(unit, fn.constInt(ty, (I64)~shifted));
			Node* masked = fn.and_(value, fn.constInt(ty, (I64)maskBits));
			Node* placed =
					lv.bitOffset ? fn.shl(masked, fn.constInt(ty, lv.bitOffset)) : masked;
			fn.store(lv.addr, fn.or_(cleared, placed));
			return;
		}
		fn.store(lv.addr, value);
	}

	Node* Emitter::offsetPtr(Function& fn, Node* base, U64 byteOff) {
		return byteOff ? fn.add(base, constSize(fn, byteOff)) : base;
	}

	void Emitter::emitMemCopy(Function& fn, Node* dst, Node* src, U32 size) {
		for (U32 i = 0; i < size;) {
			U32 w = 8;
			while (w > 1 && (i % w != 0 || i + w > size))
				w >>= 1;
			Type* ty = mod.getInt(w * 8);
			fn.store(offsetPtr(fn, dst, i), fn.load(ty, offsetPtr(fn, src, i)));
			i += w;
		}
	}

	Node* Emitter::elemStride(Function& fn, CType ptrType) {
		CType elem = pointee(ptrType);
		if (isVlaType(elem))
			return emitArrayByteSize(fn, elem);
		return constSize(fn, byteSize(elem));
	}

	Emitter::Value Emitter::emitPtrArith(Function& fn, ExprOp op, Value lhs,
																			 Value rhs) {
		if (op == ExprOp::Add) {
			if (isPointer(lhs.type) && isPointer(rhs.type)) {
				fail("invalid operands to binary '+' (two pointers)");
				return {};
			}
			Value p = isPointer(lhs.type) ? lhs : rhs;
			Value i = isPointer(lhs.type) ? rhs : lhs;
			if (!isInteger(i.type)) {
				fail("pointer arithmetic requires an integer operand");
				return {};
			}
			Node* idx = convert(fn, i.node, i.type, ctSize());
			Node* stride = elemStride(fn, p.type);
			if (!stride)
				return {};
			return {fn.add(p.node, fn.mul(idx, stride)), p.type};
		}
		if (op == ExprOp::Sub) {
			if (isPointer(lhs.type) && isPointer(rhs.type)) {
				CType pd = ctPtrDiff();
				Node* la = fn.convert(Opcode::SExt, lhs.node, irType(pd));
				Node* ra = fn.convert(Opcode::SExt, rhs.node, irType(pd));
				Node* diff = fn.sub(la, ra); // byte difference
				Node* stride = elemStride(fn, lhs.type);
				if (!stride)
					return {};
				return {fn.sdiv(diff, stride), pd};
			}
			if (!isPointer(lhs.type) || !isInteger(rhs.type)) {
				fail("invalid operands to binary '-'");
				return {};
			}
			Node* idx = convert(fn, rhs.node, rhs.type, ctSize());
			Node* stride = elemStride(fn, lhs.type);
			if (!stride)
				return {};
			return {fn.sub(lhs.node, fn.mul(idx, stride)), lhs.type};
		}
		fail("invalid operands to binary expression on a pointer");
		return {};
	}

	Node* Emitter::emitArith(Function& fn, ExprOp op, Node* l, Node* r,
													 CType ct) {
		if (isFloating(ct)) {
			switch (op) {
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
		switch (op) {
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
		if (!emitLValue(fn, e->unary.operand, lv))
			return {};
		// C99 6.5.2.4p1 / 6.5.3.1p1: the operand of ++/-- shall be a modifiable
		// lvalue, hence not const-qualified at the top level.
		if (isTopConst(lv.type)) {
			fail("cannot modify a const-qualified lvalue");
			return {};
		}
		CType type = lv.type;
		ExprOp op = e->unary.op;
		B32 isInc = op == ExprOp::PreInc || op == ExprOp::PostInc;
		B32 isPre = op == ExprOp::PreInc || op == ExprOp::PreDec;
		if (isComplexType(type)) {
			// C99 6.5.2.4/6.5.3.1: ++/-- on a complex object adds/subtracts 1,
			// i.e. (1 + 0i), to/from it, affecting only the real part.
			CType ct = completeComplex(type);
			Value oldVal = {lv.addr, ct};
			return nullptr;

		Node* result = nullptr;
		switch (e->binary.op) {
		case ExprOp::Assign:
			result = rhs;
			break;
		case ExprOp::AddAssign:
			result = fn.add(fn.get(var), rhs);
			break;
		case ExprOp::SubAssign:
			result = fn.sub(fn.get(var), rhs);
			break;
		case ExprOp::MulAssign:
			result = fn.mul(fn.get(var), rhs);
			break;
		case ExprOp::DivAssign:
			result = fn.sdiv(fn.get(var), rhs);
			break;
		case ExprOp::RemAssign:
			result = fn.srem(fn.get(var), rhs);
			break;
		case ExprOp::ShlAssign:
			result = fn.shl(fn.get(var), rhs);
			break;
		case ExprOp::ShrAssign:
			result = fn.ashr(fn.get(var), rhs);
			break;
		case ExprOp::AndAssign:
			result = fn.and_(fn.get(var), rhs);
			break;
		case ExprOp::OrAssign:
			result = fn.or_(fn.get(var), rhs);
			break;
		case ExprOp::XorAssign:
			result = fn.xor_(fn.get(var), rhs);
			break;
		default:
			fail("unsupported assignment operator");
			return nullptr;
		}
		fn.set(var, result);
		return result;
	}

	Node* Emitter::emitExpr(Function& fn, const Expr* e) {
		switch (e->kind) {
		case ExprKind::IntLit:
			return fn.constInt(i32, e->intLit.value);

		case ExprKind::Ident: {
			U32 var;
			if (!lookup(*e->ident.name, var)) {
				fail("use of undeclared identifier '" + *e->ident.name + "'");
				return nullptr;
			}
			return fn.get(var);
		}

		case ExprKind::Unary: {
			Node* operand = emitExpr(fn, e->unary.operand);
			if (!operand)
				return nullptr;
			switch (e->unary.op) {
			case ExprOp::Pos:
				return operand;
			case ExprOp::Neg:
				return fn.neg(operand);
			case ExprOp::BitNot:
				return fn.bitNot(operand);
			case ExprOp::Not:
				// !x  ==  (x == 0), as int 0/1
				return fromBool(fn, fn.eq(operand, fn.constInt(i32, 0)));
			default:
				fail("unsupported unary operator");
				return nullptr;
			}
		}

		case ExprKind::Binary: {
			if (isAssignOp(e->binary.op))
				return emitAssign(fn, e);
			if (e->binary.op == ExprOp::LogAnd || e->binary.op == ExprOp::LogOr) {
				Node* lhs = emitExpr(fn, e->binary.lhs);
				if (!lhs)
					return nullptr;
				Node* rhs = emitExpr(fn, e->binary.rhs);
				if (!rhs)
					return nullptr;
				Node* l = fromBool(fn, toBool(fn, lhs));
				Node* r = fromBool(fn, toBool(fn, rhs));
				return e->binary.op == ExprOp::LogAnd ? fn.and_(l, r) : fn.or_(l, r);
			}

			Node* lhs = emitExpr(fn, e->binary.lhs);
			if (!lhs)
				return nullptr;
			Node* rhs = emitExpr(fn, e->binary.rhs);
			if (!rhs)
				return nullptr;
			switch (e->binary.op) {
			case ExprOp::Add:
				return fn.add(lhs, rhs);
			case ExprOp::Sub:
				return fn.sub(lhs, rhs);
			case ExprOp::Mul:
				return fn.mul(lhs, rhs);
			case ExprOp::Div:
				return fn.sdiv(lhs, rhs);
			case ExprOp::Rem:
				return fn.srem(lhs, rhs);
			case ExprOp::Shl:
				return fn.shl(lhs, rhs);
			case ExprOp::Shr:
				return fn.ashr(lhs, rhs);
			case ExprOp::Lt:
				return fromBool(fn, fn.slt(lhs, rhs));
			case ExprOp::Gt:
				return fromBool(fn, fn.sgt(lhs, rhs));
			case ExprOp::Le:
				return fromBool(fn, fn.sle(lhs, rhs));
			case ExprOp::Ge:
				return fromBool(fn, fn.sge(lhs, rhs));
			case ExprOp::Eq:
				return fromBool(fn, fn.eq(lhs, rhs));
			case ExprOp::Ne:
				return fromBool(fn, fn.ne(lhs, rhs));
			case ExprOp::BitAnd:
				return fn.and_(lhs, rhs);
			case ExprOp::BitOr:
				return fn.or_(lhs, rhs);
			case ExprOp::BitXor:
				return fn.xor_(lhs, rhs);
			default:
				fail("unsupported binary operator");
				return nullptr;
			}
		}

		case ExprKind::Ternary: {
			Node* cond = emitExpr(fn, e->ternary.cond);
			if (!cond)
				return nullptr;
			Node* whenTrue = emitExpr(fn, e->ternary.whenTrue);
			if (!whenTrue)
				return nullptr;
			Node* whenFalse = emitExpr(fn, e->ternary.whenFalse);
			if (!whenFalse)
				return nullptr;
			Node* mask = fn.neg(fromBool(fn, toBool(fn, cond))); // 0 or -1
			Node* keepTrue = fn.and_(whenTrue, mask);
			Node* keepFalse = fn.and_(whenFalse, fn.bitNot(mask));
			return fn.or_(keepTrue, keepFalse);
		}

		case ExprKind::Comma: {
			Node* lhs = emitExpr(fn, e->comma.lhs);
			if (!lhs)
				return nullptr;
			return emitExpr(fn, e->comma.rhs);
		}
		}
		fail("unsupported expression");
		return nullptr;
	}

	B32 Emitter::emitStmt(Function& fn, const Stmt* s) {
		switch (s->kind) {
		case StmtKind::Compound: {
			pushScope();
			for (const Stmt* child : s->body) {
				if (fn.blockFinished())
					break; // unreachable code after a return
				if (!emitStmt(fn, child)) {
					popScope();
					return false;
				}
			}
			popScope();
			return true;
		}

		case StmtKind::Decl:
			for (const Declarator& d : s->decls) {
				Node* init = d.init ? emitExpr(fn, d.init) : fn.constInt(i32, 0);
				if (!init)
					return false;
				declare(*d.name, fn.declareLocal(*d.name, init));
			}
			return true;

		case StmtKind::Return: {
			Node* value =
					s->expr ? emitExpr(fn, s->expr) : fn.constInt(i32, 0);
			if (!value)
				return false;
			fn.ret(value);
			return true;
		}

		case StmtKind::Expr: {
			Node* value = emitExpr(fn, s->expr);
			return value != nullptr;
		}

		case StmtKind::Empty:
			return true;
		}
		fail("unsupported statement");
		return false;
	}

	B32 Emitter::emit(const TransUnit& unit) {
		i32 = mod.getInt(32);
		for (const FuncDef* def : unit.functions) {
			Function* fn = mod.createFunction(def->name, {}, i32);
			scopes.clear();
			if (def->body && !emitStmt(*fn, def->body))
				return false;
			if (!fn->blockFinished())
				fn->ret(fn->constInt(i32, 0)); // implicit return 0
		}
		return !failed;
	}
} // namespace rat::cc
