#include "emit/emit.h"

namespace rat::cc {
	Emitter::Value Emitter::emitAddrOf(Function& fn, const Expr* e) {
		if(e->unary.operand->kind == ExprKind::Ident) {
			const String& name = *e->unary.operand->ident.name;
			auto f = syms.functions.find(name);
			if(f != syms.functions.end())
				return {fn.global(name), funcPtrType(f->second)};
			Local loc;
			if(func.scopes.lookup(name, loc) && loc.isArray)
				return {loc.addr, pointerTo(loc.type)};
			auto gvo = syms.globals.find(name);
			if(gvo != syms.globals.end() && gvo->second.isArray)
				return emitExpr(fn, e->unary.operand);
		}
		LValue lv;
		if(!emitLValue(fn, e->unary.operand, lv))
			return {};
		if(lv.isVar())
			return diag.fail("cannot take the address of an SSA value");
		return {lv.addr, pointerTo(lv.type)};
	}

	Emitter::Value Emitter::emitDeref(Function& fn, const Expr* e) {
		Value p = emitExpr(fn, e->unary.operand);
		if(!p.node)
			return {};
		if(p.type.func && p.type.ptr == 1)
			return p;
		if(!isPointer(p.type)) {
			diag.offset = e->offset;
			return diag.fail("indirection requires a pointer operand");
		}
		CType pt = pointee(p.type);
		if(isArrayType(pt))
			return {p.node, decay(pt)};
		if(isAggregate(pt))
			return {p.node, pt};
		return {fn.load(irType(pt), p.node, isTopVolatile(pt)), pt};
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
		if(isComplexType(v.type) && e->unary.op == ExprOp::Pos)
			return v;
		if(isComplexType(v.type) && e->unary.op == ExprOp::Neg) {
			Node* im = fn.unary(Opcode::FNeg, complexImag(fn, v));
			Node* re = fn.unary(Opcode::FNeg, complexReal(fn, v));
			return makeComplex(fn, v.type, re, im);
		}
		switch(e->unary.op) {
		case ExprOp::Pos: {
			if(!isInteger(v.type) && !isFloating(v.type))
				return diag.fail("wrong type argument to unary plus");
			CType t = promote(v.type);
			return {convert(fn, v.node, v.type, t), t};
		}
		case ExprOp::Neg: {
			if(!isInteger(v.type) && !isFloating(v.type))
				return diag.fail("wrong type argument to unary minus");
			CType t = promote(v.type);
			Node* nv = convert(fn, v.node, v.type, t);
			if(isFloating(t))
				return {fn.unary(Opcode::FNeg, nv), t};
			return {reduceBitfield(fn, fn.neg(nv), t), t};
		}
		case ExprOp::BitNot: {
			if(!isInteger(v.type))
				return diag.fail("wrong type argument to bit-complement");
			CType t = promote(v.type);
			return {reduceBitfield(fn, fn.bitNot(convert(fn, v.node, v.type, t)), t), t};
		}
		case ExprOp::Not:
			if(isAggregate(v.type) && !isComplexType(v.type))
				return diag.fail("wrong type argument to unary exclamation mark");
			if(isComplexType(v.type))
				return {fromBool(fn, fn.eq(toBool(fn, v), fn.constInt(i32, 1))), ctInt()};
			if(isFloating(v.type)) {
				Node* zero = fn.constFloat(irType(v.type), 0.0);
				return {fromBool(fn, fn.compare(Opcode::FEq, v.node, zero)), ctInt()};
			}
			return {fromBool(fn, fn.eq(v.node, fn.constInt(irType(v.type), 0))), ctInt()};
		default:
			return diag.fail("unsupported unary operator");
		}
	}

	Emitter::Value Emitter::emitIdent(Function& fn, const Expr* e) {
		const String& name = *e->ident.name;
		Symbol sym = lookupSymbol(name);
		if(sym.kind == Symbol::Kind::None)
			return failUndeclared(name);
		if(sym.kind == Symbol::Kind::Function)
			return {fn.global(name), sym.type};
		if(sym.local && !sym.local->inMem())
			return {fn.get(sym.local->var), sym.type};
		Node* addr = sym.local ? sym.local->addr : fn.global(syms.resolveAlias(name));
		if(sym.isArray)
			return {addr, pointerTo(sym.type)};
		if(isAggregate(sym.type))
			return {addr, sym.type};
		return {fn.load(irType(sym.type), addr, isTopVolatile(sym.type)), sym.type};
	}

	Emitter::Value Emitter::emitSizeof(Function& fn, const Expr* e) {
		CType sz = ctSize();
		const Expr* operand = e->sizeOf.operand;
		U64 n;
		if(operand) {
			if(operand->kind == ExprKind::Ident) {
				Local loc;
				if(func.scopes.lookup(*operand->ident.name, loc) && loc.lengthNode)
					return {loc.lengthNode, sz};
			}
			CType ot;
			if(typeOf(operand, ot) && hasVlaDim(ot))
				return {emitArrayByteSize(fn, ot), sz};
			if(!sizeofOperand(operand, n))
				return {};
		} else {
			CType t = e->sizeOf.type;
			if(hasVlaDim(t))
				return {emitArrayByteSize(fn, t), sz};
			if(isStruct(t) && (t.strukt == nullptr || !t.strukt->complete))
				return diag.fail("sizeof applied to an incomplete type");
			n = byteSize(t);
		}
		return {constSize(fn, n), sz};
	}

	B32 Emitter::alignofValue(const Expr* e, U32& out) {
		if(!e->sizeOf.operand) {
			out = alignOf(e->sizeOf.type);
			return true;
		}
		if(e->sizeOf.operand->kind == ExprKind::Ident) {
			auto fit = syms.functions.find(*e->sizeOf.operand->ident.name);
			if(fit != syms.functions.end()) {
				out = fit->second.align ? fit->second.align : 1u;
				return true;
			}
		}
		CType ot;
		if(!typeOf(e->sizeOf.operand, ot))
			return false;
		out = alignOf(ot);
		return true;
	}

	Emitter::Value Emitter::emitStmtExpr(Function& fn, const Expr* e) {
		const Stmt* body = e->stmtExpr.body;
		const List<Stmt*>& stmts = body->body;
		func.scopes.push();
		Value result{};
		for(U32 i = 0; i < stmts.size(); ++i) {
			const Stmt* child = stmts[i];
			B32 last = (i + 1 == stmts.size());
			if(last && child->kind == StmtKind::Expr && !fn.blockFinished()) {
				result = emitExpr(fn, child->expr);
				if(!result.node)
					return {};
				break;
			}
			if(fn.blockFinished() && !detail::isLabeledStmt(child) && !containsLabel(child)) {
				if(child->kind == StmtKind::Decl && !declareDead(fn, child))
					return {};
				continue;
			}
			if(!emitStmt(fn, child))
				return {};
		}
		func.scopes.pop();
		if(!result.node)
			return {fn.constInt(i32, 0), ctInt()};
		return result;
	}

	Emitter::Value Emitter::emitExpr(Function& fn, const Expr* e) {
		diag.offset = e->offset;
		switch(e->kind) {
		case ExprKind::IntLit: {
			CType t;
			t.bits = e->intLit.bits;
			t.mods = e->intLit.mods;
			return {fn.constInt(irType(t), e->intLit.value), t};
		}
		case ExprKind::FloatLit: {
			CType t;
			t.base = CType::Base::Float;
			t.bits = e->floatLit.bits;
			Node* lit = floatLit(fn, t, e->floatLit.value);
			if(e->floatLit.imaginary) {
				CType ct = t;
				ct.set(CType::Complex);
				return makeComplex(fn, ct, fn.constFloat(irType(t), 0.0), lit);
			}
			return {lit, t};
		}
		case ExprKind::StrLit: {
			CType elemPtr;
			elemPtr.bits = e->str.isWide ? e->str.charSize * 8 : 8;
			elemPtr.ptr = 1;
			return {fn.global(internString(e)), elemPtr};
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
			if(isVoidType(castTy))
				return {v.node, castTy};
			if(!isAggregate(castTy) && isAggregate(v.type) && !isComplexType(v.type))
				return diag.fail("cannot cast a struct or union to a scalar type");
			if(isPointer(v.type) && !isPointer(castTy) && !isFloating(castTy) && !castTy.isVoid()) {
				if(castTy.bits == 1)
					return {fn.ne(v.node, fn.constInt(mod.getPtr(), 0)), castTy};
				if(castTy.bits < 64)
					return {fn.trunc(v.node, irType(castTy)), castTy};
			}
			return {convert(fn, v.node, v.type, castTy), castTy};
		}
		case ExprKind::Sizeof:
			return emitSizeof(fn, e);
		case ExprKind::AlignOf: {
			U32 n;
			if(!alignofValue(e, n))
				return {};
			return {constSize(fn, n), ctSize()};
		}
		case ExprKind::VaArg: {
			Node* ap = vaListRef(fn, e->vaArg.ap);
			if(!ap)
				return {};
			CType fetched = completeComplex(e->vaArg.type);
			if(isAggregate(fetched))
				return emitVaArgAggregate(fn, ap, fetched);
			return {fn.call("__builtin_va_arg", irType(e->vaArg.type), {ap}), e->vaArg.type};
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
			if(isArrayType(lv.type))
				return {lv.addr, decay(lv.type)};
			if(isStruct(lv.type))
				return {lv.addr, lv.type};
			return {loadLValue(fn, lv), lv.type};
		}
		case ExprKind::InitList:
			return diag.fail("initializer list is only allowed in a declaration");
		case ExprKind::CompoundLit:
			return emitCompoundLit(fn, e);
		}
		return diag.fail("unsupported expression");
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
		Block* tB = fn.createBlock("tern.true");
		Block* fB = fn.createBlock("tern.false");
		Block* eB = fn.createBlock("tern.end");
		fn.jumpif(pred, tB);
		fn.jmp(fB);
		const Expr* arms[] = {e->ternary.whenTrue, e->ternary.whenFalse};
		Block* armBlocks[] = {tB, fB};
		for(U32 i = 0; i < 2; ++i) {
			fn.enterBlock(armBlocks[i]);
			Value v = emitExpr(fn, arms[i]);
			if(!v.node)
				return {};
			if(!fn.blockFinished()) {
				if(!isVoidRes)
					fn.set(var, convert(fn, v.node, v.type, rt));
				fn.jmp(eB);
			}
		}
		fn.enterBlock(eB);
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
				if(!evalConst(e->compound.arrayLen, count))
					return failArrayCount();
			} else if(init->kind == ExprKind::StrLit) {
				count = (I64)init->str.bytes->size() + 1;
			} else if(init->kind == ExprKind::InitList) {
				List<I64> idx;
				if(!resolveArrayIndices(init, false, count, idx))
					return {};
			}
			if(count <= 0)
				return failArrayCount();
			U32 total = (U32)count * elemSize;
			Node* slot;
			if(isAggregate(ty))
				slot = allocBytes(fn, total);
			else
				slot = fn.alloc(mod.getArray(elemTy, (U32)count));
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
		B32 skip = false;
		if(!unwrapScalarInit(se, skip))
			return {};
		if(skip)
			return {fn.constInt(irType(ty), 0), ty};
		Value v = emitExpr(fn, se);
		if(!v.node)
			return {};
		return {convert(fn, v.node, v.type, ty), ty};
	}
} // namespace rat::cc
