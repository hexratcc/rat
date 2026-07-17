#include "Emit/Emit.h"

namespace rat::cc {
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
			return {fn.constInt(i32, 0), ctInt()};
		return result;
	}

	Emitter::Value Emitter::emitExpr(Function& fn, const Expr* e) {
		curOffset = e->offset;
		switch(e->kind) {
		case ExprKind::IntLit: {
			CType t;
			t.isUnsigned = e->intLit.isUnsigned;
			t.bits = e->intLit.bits;
			t.isLong = e->intLit.isLong;
			t.isLongLong = e->intLit.isLongLong;
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
			Node* ap = vaListRef(fn, e->vaArg.ap);
			if(!ap)
				return {};
			Type* fetched = isStruct(e->vaArg.type) ? mod.getPtr() : irType(e->vaArg.type);
			Node* r = fn.call("__builtin_va_arg", fetched, {ap});
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
} // namespace rat::cc
