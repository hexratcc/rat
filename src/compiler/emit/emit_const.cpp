#include "emit/emit.h"

namespace rat::cc {
	// an unsized array compound literal and an aligned function, else from the type
	B32 Emitter::ConstFolder::sizeOf(const Expr* e, U64& out) const {
		const Expr* operand = e->sizeOf.operand;
		if(operand && e->kind == ExprKind::Sizeof)
			return emit.sizeofOperand(operand, out);
		if(operand && operand->kind == ExprKind::Ident) {
			auto fit = emit.syms.functions.find(*operand->ident.name);
			if(fit != emit.syms.functions.end() && fit->second.align) {
				out = fit->second.align;
				return true;
			}
		}
		return ConstEval::sizeOf(e, out);
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
			memberField(lv, st, f);
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
			if(found.kind != Symbol::Kind::Function && !isArrayType(found.type))
				return false;
			return addrConstOf(e, sym, addend);
		}
		case ExprKind::Member: {
			CType st;
			const Field* f;
			memberField(e, st, f);
			if(!f || !isStruct(st) || !isArrayType(f->type))
				return false;
			return addrConstOf(e, sym, addend);
		}
		case ExprKind::Unary: {
			if(e->unary.op == ExprOp::Addr)
				return addrConstOf(e->unary.operand, sym, addend);
			if(e->unary.op == ExprOp::Deref && isArrayType(e->type))
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
			if(!folder.evalInt(intSide, n))
				return false;
			CType pt = valueType(ptrSide->type);
			I64 scale = 1;
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
