#include "emit/emit.h"

namespace rat::cc {
	namespace detail {
		I64 narrowToType(I64 v, CType ty) {
			U32 bits = isPointer(ty) ? 64 : ty.bits;
			return wrapInt(v, bits, ty.isUnsigned());
		}

		CType arithResultType(CType a, CType b) {
			CType r = ctInt();
			r.bits = a.bits >= b.bits ? a.bits : b.bits;
			if(r.bits < 32)
				r.bits = 32; // int promotion of sub-int operands
			B32 au = a.isUnsigned() && a.bits >= 32;
			B32 bu = b.isUnsigned() && b.bits >= 32;
			if(a.bits == b.bits)
				r.set(CType::Unsigned, au || bu);
			else
				r.set(CType::Unsigned, (a.bits > b.bits) ? au : bu);
			return r;
		}
	} // namespace detail

	B32 Emitter::evalConstUnary(ExprOp op, I64 v, CType opTy, I64& out, CType& ty) {
		ty = op == ExprOp::Not ? ctInt() : opTy;
		switch(op) {
			// clang-format off
		case ExprOp::Pos:    out = v;  return true;
		case ExprOp::Neg:    out = (I64)(0 - (U64)v); return true; // wraps at INT64_MIN
		case ExprOp::BitNot: out = ~v; return true;
		case ExprOp::Not:    out = !v; return true;
		// clang-format on
		default:
			return false;
		}
	}

	B32 Emitter::evalConstBinary(ExprOp op, I64 a, CType aTy, I64 b, CType bTy, I64& out, CType& ty) {
		B32 ap = isPointer(aTy), bp = isPointer(bTy);
		if((op == ExprOp::Add || op == ExprOp::Sub) && (ap || bp)) {
			if(ap && bp) { // ptr - ptr
				U32 esz = byteSize(pointee(aTy));
				if(!esz)
					return false;
				out = (a - b) / (I64)esz;
				ty = ctPtrDiff();
				return true;
			}
			CType pTy = ap ? aTy : bTy;
			I64 pv = ap ? a : b, iv = ap ? b : a;
			U32 esz = byteSize(pointee(pTy));
			out = op == ExprOp::Sub ? pv - iv * (I64)esz : pv + iv * (I64)esz;
			ty = pTy;
			return true;
		}
		switch(op) {
		case ExprOp::LogAnd:
		case ExprOp::LogOr:
			if(op == ExprOp::LogAnd)
				out = a && b;
			else
				out = a || b;
			ty = ctInt();
			return true;
		case ExprOp::Shl:
		case ExprOp::Shr: {
			ty = aTy;
			if(ty.bits < 32)
				ty.bits = 32;
			if(b < 0 || b >= (I64)ty.bits)
				return false;
			if(op == ExprOp::Shl)
				out = detail::narrowToType((I64)((U64)a << (U64)b), ty);
			else if(ty.isUnsigned())
				out = (I64)((U64)detail::narrowToType(a, ty) >> (U64)b);
			else
				out = a >> b;
			return true;
		}
		default:
			break;
		}
		CType rt = detail::arithResultType(aTy, bTy);
		U64 ua = (U64)detail::narrowToType(a, rt), ub = (U64)detail::narrowToType(b, rt);
		B32 u = rt.isUnsigned();
		ty = (op >= ExprOp::Lt && op <= ExprOp::Ne) ? ctInt() : rt;
		switch(op) {
			// clang-format off
		case ExprOp::Add:    out = detail::narrowToType((I64)((U64)a + (U64)b), rt); return true;
		case ExprOp::Sub:    out = detail::narrowToType((I64)((U64)a - (U64)b), rt); return true;
		case ExprOp::Mul:    out = detail::narrowToType((I64)((U64)a * (U64)b), rt); return true;
		case ExprOp::BitAnd: out = detail::narrowToType(a & b, rt); return true;
		case ExprOp::BitOr:  out = detail::narrowToType(a | b, rt); return true;
		case ExprOp::BitXor: out = detail::narrowToType(a ^ b, rt); return true;
		case ExprOp::Lt:     out = u ? ua < ub : a < b;     return true;
		case ExprOp::Gt:     out = u ? ua > ub : a > b;     return true;
		case ExprOp::Le:     out = u ? ua <= ub : a <= b;   return true;
		case ExprOp::Ge:     out = u ? ua >= ub : a >= b;   return true;
		case ExprOp::Eq:     out = a == b;                  return true;
		case ExprOp::Ne:     out = a != b;                  return true;
		// clang-format on
		case ExprOp::Div:
		case ExprOp::Rem:
			if(!b || (!u && a == INT64_MIN && b == -1))
				return false;
			if(op == ExprOp::Div)
				out = u ? (I64)(ua / ub) : detail::narrowToType(a / b, rt);
			else
				out = u ? (I64)(ua % ub) : detail::narrowToType(a % b, rt);
			return true;
		default:
			return false;
		}
	}

	B32 Emitter::evalConstTyped(const Expr* e, I64& out, CType& ty) {
		switch(e->kind) {
		case ExprKind::IntLit:
			out = e->intLit.value;
			ty = ctInt();
			ty.bits = e->intLit.bits;
			ty.mods = e->intLit.mods;
			return true;
		case ExprKind::Sizeof: {
			if(e->sizeOf.operand) {
				U64 sz;
				if(!sizeofOperand(e->sizeOf.operand, sz))
					return false;
				out = sz;
			} else if(hasVlaDim(e->sizeOf.type)) {
				return false;
			} else {
				out = byteSize(e->sizeOf.type);
			}
			ty = ctSize();
			return true;
		}
		case ExprKind::AlignOf: {
			U32 n;
			if(!alignofValue(e, n))
				return false;
			out = n;
			ty = ctSize();
			return true;
		}
		case ExprKind::Cast: {
			ty = e->cast.type;
			if(!resolveType(ty)) // typeof(expr) casts
				return false;
			CType opTy;
			if(!evalConstTyped(e->cast.operand, out, opTy)) {
				F80 fv;
				if(isFloating(ty) || isPointer(ty) || isAggregate(ty) || ty.isVoid())
					return false;
				if(!evalFloatConst(e->cast.operand, fv))
					return false;
				out = (I64)fv;
			}
			out = detail::narrowToType(out, ty);
			return true;
		}
		case ExprKind::Unary: {
			CType opTy;
			I64 v;
			if(!evalConstTyped(e->unary.operand, v, opTy))
				return false;
			return evalConstUnary(e->unary.op, v, opTy, out, ty);
		}
		case ExprKind::Binary: {
			CType aTy, bTy;
			I64 a, b;
			if(!evalConstTyped(e->binary.lhs, a, aTy) || !evalConstTyped(e->binary.rhs, b, bTy))
				return false;
			return evalConstBinary(e->binary.op, a, aTy, b, bTy, out, ty);
		}
		case ExprKind::Ternary: {
			CType cTy;
			I64 c;
			if(!evalConstTyped(e->ternary.cond, c, cTy))
				return false;
			return evalConstTyped(c ? e->ternary.whenTrue : e->ternary.whenFalse, out, ty);
		}
		case ExprKind::Comma:
			return evalConstTyped(e->comma.rhs, out, ty);
		case ExprKind::Call: {
			if(!e->call.callee || *e->call.callee != "__builtin_classify_type" || e->args.size() != 1)
				return false;
			CType opTy;
			if(!typeOf(e->args[0], opTy))
				return false;
			out = typeClassOf(opTy);
			ty = ctInt();
			return true;
		}
		default:
			return false;
		}
	}

	B32 Emitter::evalConst(const Expr* e, I64& out) {
		CType ty;
		return evalConstTyped(e, out, ty);
	}

	B32 Emitter::evalFloatConst(const Expr* e, F80& out) {
		if(e->kind == ExprKind::IntLit || e->kind == ExprKind::Cast) {
			I64 iv;
			CType it;
			if(evalConstTyped(e, iv, it) && isInteger(it)) {
				out = it.isUnsigned() ? (F80)(U64)iv : (F80)iv;
				return true;
			}
		}
		switch(e->kind) {
		case ExprKind::FloatLit:
			out = e->floatLit.value;
			return true;
		case ExprKind::Cast:
			return evalFloatConst(e->cast.operand, out);
		case ExprKind::Unary: {
			F80 v;
			ExprOp op = e->unary.op;
			if(!evalFloatConst(e->unary.operand, v) || (op != ExprOp::Pos && op != ExprOp::Neg))
				return false;
			out = op == ExprOp::Neg ? -v : v;
			return true;
		}
		case ExprKind::Binary: {
			F80 a, b;
			if(!evalFloatConst(e->binary.lhs, a) || !evalFloatConst(e->binary.rhs, b))
				return false;
			switch(e->binary.op) {
				// clang-format off
			case ExprOp::Add: out = a + b; return true;
			case ExprOp::Sub: out = a - b; return true;
			case ExprOp::Mul: out = a * b; return true;
			case ExprOp::Div: out = a / b; return true;
			// clang-format on
			default:
				return false;
			}
		}
		case ExprKind::Ternary: {
			B32 taken;
			I64 ci;
			if(evalConst(e->ternary.cond, ci)) {
				taken = ci != 0;
			} else {
				F80 cf;
				if(!evalFloatConst(e->ternary.cond, cf))
					return false;
				taken = cf != 0;
			}
			return evalFloatConst(taken ? e->ternary.whenTrue : e->ternary.whenFalse, out);
		}
		default:
			return false;
		}
	}

	B32 Emitter::addrConstOf(const Expr* lv, String& sym, I64& addend) {
		addend = 0;
		switch(lv->kind) {
		case ExprKind::Ident: {
			const String& name = *lv->ident.name;
			Symbol found = lookupSymbol(name);
			if(found.local) {
				if(!found.local->staticSymbol)
					return false;
				sym = *found.local->staticSymbol;
				return true;
			}
			if(found.kind == Symbol::Kind::None)
				return false;
			sym = syms.resolveAlias(name);
			return true;
		}
		case ExprKind::Member: {
			CType st;
			const Field* f;
			if(!memberField(lv, st, f) || !f)
				return false;
			I64 base = 0;
			if(lv->member.arrow) {
				if(!evalAddrConst(lv->member.base, sym, base))
					return false;
			} else if(!addrConstOf(lv->member.base, sym, base))
				return false;
			addend = base + (I64)f->offset;
			return true;
		}
		case ExprKind::Unary:
			if(lv->unary.op == ExprOp::Deref)
				return evalAddrConst(lv->unary.operand, sym, addend);
			return false;
		case ExprKind::CompoundLit:
			return internCompoundLiteral(lv, sym);
		default:
			return false;
		}
	}

	B32 Emitter::evalAddrConst(const Expr* e, String& sym, I64& addend) {
		addend = 0;
		switch(e->kind) {
		case ExprKind::StrLit:
			sym = internString(e);
			return true;
		case ExprKind::CompoundLit:
			return isArrayType(e->compound.type) && internCompoundLiteral(e, sym);
		case ExprKind::Cast:
			return evalAddrConst(e->cast.operand, sym, addend);
		case ExprKind::Ident: {
			Symbol found = lookupSymbol(*e->ident.name);
			if(found.local) {
				if(!found.local->staticSymbol || !isArrayType(found.type))
					return false;
				sym = *found.local->staticSymbol;
				return true;
			}
			if(found.kind != Symbol::Kind::Function && !isArrayType(found.type))
				return false;
			sym = *e->ident.name;
			return true;
		}
		case ExprKind::Member: {
			CType st;
			const Field* f;
			if(!memberField(e, st, f) || !f || !isStruct(st) || !isArrayType(f->type))
				return false;
			return addrConstOf(e, sym, addend);
		}
		case ExprKind::Unary: {
			if(e->unary.op == ExprOp::Addr)
				return addrConstOf(e->unary.operand, sym, addend);
			CType t;
			if(e->unary.op == ExprOp::Deref && typeOf(e, t) && isArrayType(t))
				return evalAddrConst(e->unary.operand, sym, addend);
			return false;
		}
		case ExprKind::Binary: {
			if(e->binary.op != ExprOp::Add && e->binary.op != ExprOp::Sub)
				return false;
			const Expr* ptrSide = e->binary.lhs;
			const Expr* intSide = e->binary.rhs;
			if(!evalAddrConst(ptrSide, sym, addend)) {
				if(e->binary.op == ExprOp::Sub)
					return false;
				ptrSide = e->binary.rhs;
				intSide = e->binary.lhs;
				if(!evalAddrConst(ptrSide, sym, addend))
					return false;
			}
			I64 n = 0;
			if(!evalConst(intSide, n))
				return false;
			CType pt;
			I64 scale = 1;
			if(typeOf(ptrSide, pt) && isArrayType(pt))
				pt = decay(pt);
			if(isPointer(pt))
				scale = (I64)byteSize(pointee(pt));
			addend += (e->binary.op == ExprOp::Sub ? -n : n) * scale;
			return true;
		}
		default:
			return false;
		}
	}
} // namespace rat::cc
