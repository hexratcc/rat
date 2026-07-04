#include "Emit.h"

namespace rat::cc {
	static I64 narrowToType(I64 v, CType ty) {
		U32 bits = isPointer(ty) ? 64 : ty.bits;
		if(bits >= 64)
			return v;
		U64 mask = ((U64)1 << bits) - 1;
		U64 low = (U64)v & mask;
		if(!ty.isUnsigned && (low & ((U64)1 << (bits - 1))))
			low |= ~mask; // sign-extend
		return (I64)low;
	}

	static CType arithResultType(CType a, CType b) {
		CType r = ctInt();
		r.bits = a.bits >= b.bits ? a.bits : b.bits;
		if(r.bits < 32)
			r.bits = 32; // int promotion of sub-int operands
		B32 au = a.isUnsigned && a.bits >= 32;
		B32 bu = b.isUnsigned && b.bits >= 32;
		if(a.bits == b.bits)
			r.isUnsigned = au || bu;
		else
			r.isUnsigned = (a.bits > b.bits) ? au : bu;
		return r;
	}

	B32 Emitter::evalConstUnary(ExprOp op, I64 v, CType opTy, I64& out, CType& ty) {
		switch(op) {
		case ExprOp::Pos:
			out = v;
			ty = opTy;
			return true;
		case ExprOp::Neg:
			out = -v;
			ty = opTy;
			return true;
		case ExprOp::BitNot:
			out = ~v;
			ty = opTy;
			return true;
		case ExprOp::Not:
			out = !v;
			ty = ctInt();
			return true;
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
				ty = ctSize();
				ty.isUnsigned = false; // ptrdiff_t is signed
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
			out = a && b;
			ty = ctInt();
			return true;
		case ExprOp::LogOr:
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
				out = narrowToType((I64)((U64)a << (U64)b), ty);
			else if(ty.isUnsigned)
				out = (I64)((U64)narrowToType(a, ty) >> (U64)b);
			else
				out = a >> b;
			return true;
		}
		default:
			break;
		}
		CType rt = arithResultType(aTy, bTy);
		U64 ua = (U64)narrowToType(a, rt), ub = (U64)narrowToType(b, rt);
		B32 u = rt.isUnsigned;
		switch(op) {
		case ExprOp::Add:
			out = narrowToType(a + b, rt);
			ty = rt;
			return true;
		case ExprOp::Sub:
			out = narrowToType(a - b, rt);
			ty = rt;
			return true;
		case ExprOp::Mul:
			out = narrowToType(a * b, rt);
			ty = rt;
			return true;
		case ExprOp::Div:
			if(!b)
				return false;
			out = u ? (I64)(ua / ub) : narrowToType(a / b, rt);
			ty = rt;
			return true;
		case ExprOp::Rem:
			if(!b)
				return false;
			out = u ? (I64)(ua % ub) : narrowToType(a % b, rt);
			ty = rt;
			return true;
		case ExprOp::BitAnd:
			out = narrowToType(a & b, rt);
			ty = rt;
			return true;
		case ExprOp::BitOr:
			out = narrowToType(a | b, rt);
			ty = rt;
			return true;
		case ExprOp::BitXor:
			out = narrowToType(a ^ b, rt);
			ty = rt;
			return true;
		case ExprOp::Lt:
			out = u ? ua < ub : a < b;
			ty = ctInt();
			return true;
		case ExprOp::Gt:
			out = u ? ua > ub : a > b;
			ty = ctInt();
			return true;
		case ExprOp::Le:
			out = u ? ua <= ub : a <= b;
			ty = ctInt();
			return true;
		case ExprOp::Ge:
			out = u ? ua >= ub : a >= b;
			ty = ctInt();
			return true;
		case ExprOp::Eq:
			out = a == b;
			ty = ctInt();
			return true;
		case ExprOp::Ne:
			out = a != b;
			ty = ctInt();
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
			ty.isUnsigned = e->intLit.isUnsigned;
			ty.bits = e->intLit.isLong ? 64 : 32;
			return true;
		case ExprKind::Sizeof: {
			if(e->sizeOf.operand) {
				U32 sz;
				if(!sizeofOperand(e->sizeOf.operand, sz))
					return false;
				out = sz;
			} else {
				out = byteSize(e->sizeOf.type);
			}
			ty = ctSize();
			return true;
		}
		case ExprKind::Cast: {
			ty = e->cast.type;
			if(!resolveType(ty)) // typeof(expr) casts
				return false;
			CType opTy;
			if(!evalConstTyped(e->cast.operand, out, opTy)) {
				long double fv;
				if(isFloating(ty) || isPointer(ty) || isAggregate(ty) || ty.isVoid)
					return false;
				if(!evalFloatConst(e->cast.operand, fv))
					return false;
				out = (I64)fv;
			}
			out = narrowToType(out, ty);
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
		default:
			return false;
		}
	}

	B32 Emitter::evalConst(const Expr* e, I64& out) {
		CType ty;
		return evalConstTyped(e, out, ty);
	}

	B32 Emitter::evalFloatConst(const Expr* e, long double& out) {
		switch(e->kind) {
		case ExprKind::FloatLit:
			out = e->floatLit.value;
			return true;
		case ExprKind::IntLit:
			out = (long double)e->intLit.value;
			return true;
		case ExprKind::Cast:
			return evalFloatConst(e->cast.operand, out);
		case ExprKind::Unary: {
			long double v;
			if(!evalFloatConst(e->unary.operand, v))
				return false;
			switch(e->unary.op) {
			case ExprOp::Pos:
				out = v;
				return true;
			case ExprOp::Neg:
				out = -v;
				return true;
			default:
				return false;
			}
		}
		case ExprKind::Binary: {
			long double a, b;
			if(!evalFloatConst(e->binary.lhs, a) || !evalFloatConst(e->binary.rhs, b))
				return false;
			switch(e->binary.op) {
			case ExprOp::Add:
				out = a + b;
				return true;
			case ExprOp::Sub:
				out = a - b;
				return true;
			case ExprOp::Mul:
				out = a * b;
				return true;
			case ExprOp::Div:
				out = a / b;
				return true;
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
				long double cf;
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
		switch(lv->kind) {
		case ExprKind::Ident: {
			const String& n = *lv->ident.name;
			if(globals.count(n) || globalArrays.count(n) || funcs.count(n)) {
				sym = n;
				addend = 0;
				return true;
			}
			return false;
		}
		case ExprKind::Member: {
			CType bt;
			if(!typeOf(lv->member.base, bt))
				return false;
			const StructType* st = lv->member.arrow ? pointee(bt).strukt : bt.strukt;
			if(!st)
				return false;
			const Field* f = st->find(*lv->member.name);
			if(!f)
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
		case ExprKind::CompoundLit: {
			if(!internCompoundLiteral(lv, sym))
				return false;
			addend = 0;
			return true;
		}
		default:
			return false;
		}
	}

	B32 Emitter::evalAddrConst(const Expr* e, String& sym, I64& addend) {
		switch(e->kind) {
		case ExprKind::StrLit:
			sym = internString(e);
			addend = 0;
			return true;
		case ExprKind::CompoundLit:
			if(!e->compound.isArray)
				return false;
			if(!internCompoundLiteral(e, sym))
				return false;
			addend = 0;
			return true;
		case ExprKind::Cast:
			return evalAddrConst(e->cast.operand, sym, addend);
		case ExprKind::Ident:
			if(funcs.count(*e->ident.name) || globalArrays.count(*e->ident.name)) {
				sym = *e->ident.name;
				addend = 0;
				return true;
			}
			return false;
		case ExprKind::Member: {
			CType base;
			if(!typeOf(e->member.base, base))
				return false;
			CType st = e->member.arrow ? pointee(base) : base;
			if(!isStruct(st))
				return false;
			const Field* f = st.strukt->find(*e->member.name);
			if(!f || !f->isArray)
				return false;
			return addrConstOf(e, sym, addend);
		}
		case ExprKind::Unary:
			if(e->unary.op == ExprOp::Addr)
				return addrConstOf(e->unary.operand, sym, addend);
			return false;
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
			if(typeOf(ptrSide, pt) && isPointer(pt))
				scale = (I64)byteSize(pointee(pt));
			addend += (e->binary.op == ExprOp::Sub ? -n : n) * scale;
			return true;
		}
		default:
			return false;
		}
	}
} // namespace rat::cc
