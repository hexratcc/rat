#include "emit/emit.h"

namespace rat::cc {
	Emitter::Value Emitter::emitAddrOf(Function& fn, const Expr* e) {
		const Expr* operand = e->unary.operand;
		if(operand->kind == ExprKind::Ident &&
			 lookupSymbol(*operand->ident.name).kind == Symbol::Kind::Function)
			return emitExpr(fn, operand);
		LValue lv;
		if(!emitLValue(fn, operand, lv))
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
		return loadObject(fn, p.node, pointee(p.type));
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

	Emitter::Value Emitter::loadObject(Function& fn, Node* addr, CType type) {
		if(isArrayType(type))
			return {addr, decay(type)};
		if(isAggregate(type))
			return {addr, type};
		return {fn.load(irType(type), addr, isTopVolatile(type)), type};
	}

	Emitter::Value Emitter::emitIdent(Function& fn, const Expr* e) {
		const String& name = *e->ident.name;
		Symbol sym = lookupSymbol(name);
		if(sym.kind == Symbol::Kind::None)
			return failUndeclared(name);
		return symbolValue(fn, name, sym);
	}

	Emitter::Value Emitter::symbolValue(Function& fn, const String& name, const Symbol& sym) {
		if(sym.kind == Symbol::Kind::Function)
			return {fn.global(name), sym.type};
		if(sym.local && !sym.local->inMem())
			return {fn.get(sym.local->var), sym.type};
		Node* addr = sym.local ? sym.local->addr : fn.global(syms.resolveAlias(name));
		return loadObject(fn, addr, sym.type);
	}

	Emitter::Value Emitter::emitSizeof(Function& fn, const Expr* e) {
		CType sz = sizeType(lay);
		const Expr* operand = e->sizeOf.operand;
		CType t = e->sizeOf.type;
		if(operand)
			t = operand->type;
		if(operand && operand->kind == ExprKind::Ident) {
			Local loc;
			if(func.scopes.lookup(*operand->ident.name, loc) && loc.lengthNode)
				return {loc.lengthNode, sz};
		}
		if(hasVlaDim(t))
			return {emitArrayByteSize(fn, t), sz};
		U64 n;
		if(!folder.sizeOf(e, n))
			return diag.fail("sizeof applied to an incomplete type");
		return {constSize(fn, n), sz};
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
				if(child->kind == StmtKind::Decl && !emitDecls(fn, child, false))
					return {};
				continue;
			}
			if(!emitStmt(fn, child))
				return {};
		}
		func.scopes.pop();
		if(!result.node)
			return voidValue(fn);
		return result;
	}

	Emitter::Value Emitter::emitExpr(Function& fn, const Expr* e) {
		diag.offset = e->offset;
		switch(e->kind) {
		case ExprKind::IntLit:
			return {fn.constInt(irType(e->type), e->intLit.value), e->type};
		case ExprKind::FloatLit: {
			if(!e->floatLit.imaginary)
				return {floatLit(fn, e->type, e->floatLit.value), e->type};
			CType elem = complexElem(e->type);
			Node* im = floatLit(fn, elem, e->floatLit.value);
			return makeComplex(fn, e->type, fn.constFloat(irType(elem), 0.0), im);
		}
		case ExprKind::StrLit:
			return {fn.global(internString(e)), decay(e->type)};
		case ExprKind::Ident:
			return emitIdent(fn, e);
		case ExprKind::Call:
			return emitCall(fn, e);
		case ExprKind::Cast: {
			CType castTy = e->cast.type;
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
			U64 n;
			folder.sizeOf(e, n);
			return {constSize(fn, n), sizeType(lay)};
		}
		case ExprKind::VaArg: {
			Node* ap = vaListRef(fn, e->vaArg.ap);
			if(!ap)
				return {};
			CType fetched = e->vaArg.type;
			if(isAggregate(fetched))
				return emitVaArgAggregate(fn, ap, fetched);
			return {fn.call("__builtin_va_arg", irType(e->vaArg.type), {ap}), e->vaArg.type};
		}
		case ExprKind::StmtExpr:
			return emitStmtExpr(fn, e);
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
			if(lv.isBitfield)
				return {loadLValue(fn, lv), lv.type};
			return loadObject(fn, lv.addr, lv.type);
		}
		case ExprKind::InitList:
			return diag.fail("initializer list is only allowed in a declaration");
		case ExprKind::CompoundLit:
			return emitCompoundLit(fn, e);
		}
		return diag.fail("unsupported expression");
	}

	Emitter::Value Emitter::emitTernary(Function& fn, const Expr* e) {
		CType rt = e->type; // see ternaryType
		B32 isVoidRes = isVoidType(rt);
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
		if(isArrayType(ty)) {
			CType elem = arrayElem(ty);
			U32 elemSize = byteSize(elem);
			I64 count;
			if(!compoundArrayCount(e, count))
				return failArrayCount();
			U32 total = (U32)count * elemSize;
			Node* slot;
			if(isAggregate(elem) || isArrayType(elem))
				slot = allocBytes(fn, total);
			else
				slot = fn.alloc(mod.getArray(irType(elem), (U32)count));
			zeroSlot(fn, slot, total);
			StoreSink sink(*this, fn, slot);
			if(!initArrayInit(sink, 0, elem, (U32)count, init))
				return {};
			return {slot, pointerTo(elem)};
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
