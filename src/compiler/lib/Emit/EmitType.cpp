#include "Emit/Emit.h"

namespace rat::cc {
	B32 Emitter::resolveType(CType& t) {
		if(!t.typeofExpr)
			return true;
		U32 quals = t.quals;
		const Expr* operand = t.typeofExpr;
		if(!typeOf(operand, t))
			return false;
		t.quals |= quals;
		t.typeofExpr = nullptr;
		return true;
	}

	B32 Emitter::sizeofOperand(const Expr* operand, U32& out) {
		if(operand->kind == ExprKind::Ident) {
			Local loc;
			if(lookup(*operand->ident.name, loc) && loc.isArray) {
				if(loc.lengthNode)
					return false;
				out = loc.count * byteSize(loc.type);
				return true;
			}
			auto gv = globalVars.find(*operand->ident.name);
			if(gv != globalVars.end() && gv->second.isArray) {
				out = gv->second.count * byteSize(gv->second.type);
				return true;
			}
		}
		if(operand->kind == ExprKind::Member) {
			CType base;
			if(!typeOf(operand->member.base, base))
				return false;
			CType st = operand->member.arrow ? pointee(base) : base;
			if(isStruct(st)) {
				const Field* f = st.strukt->find(*operand->member.name);
				if(f && f->isArray()) {
					out = f->count * byteSize(f->type);
					return true;
				}
			}
		}
		CType t;
		if(!typeOf(operand, t))
			return false;
		out = byteSize(t);
		return true;
	}

	static B32 funcTypesMatch(const FuncType* a, const FuncType* b);

	static B32 genericTypesMatch(const CType& a, const CType& b) {
		if(a.ptr != b.ptr)
			return false;
		if(a.quals != b.quals)
			return false;
		if((a.func != nullptr) != (b.func != nullptr))
			return false;
		if(a.func && b.func)
			return funcTypesMatch(a.func, b.func);
		B32 aArr = a.array != nullptr && a.ptr == 0;
		B32 bArr = b.array != nullptr && b.ptr == 0;
		if(aArr != bArr)
			return false;
		if(aArr && bArr) {
			if(a.array->count != b.array->count)
				return false;
			return genericTypesMatch(a.array->elem, b.array->elem);
		}
		if((a.strukt != nullptr) != (b.strukt != nullptr))
			return false;
		if(a.strukt && b.strukt)
			return a.strukt == b.strukt;
		if(a.isVoid() != b.isVoid())
			return false;
		if(a.isFloat() != b.isFloat())
			return false;
		if(a.bits != b.bits)
			return false;
		if(!a.isFloat() && !a.isVoid()) {
			if(a.isUnsigned() != b.isUnsigned())
				return false;
			if(a.bits == 8 && a.isPlainChar() != b.isPlainChar())
				return false;
			if(a.bits == 32 && a.isLong() != b.isLong())
				return false;
			if(a.bits == 64 && a.isLongLong() != b.isLongLong())
				return false;
		}
		return true;
	}

	static B32 funcTypesMatch(const FuncType* a, const FuncType* b) {
		if(a->isVarArgs != b->isVarArgs)
			return false;
		if(a->params.size() != b->params.size())
			return false;
		if(!genericTypesMatch(a->ret, b->ret))
			return false;
		for(U32 i = 0; i < a->params.size(); ++i)
			if(!genericTypesMatch(a->params[i], b->params[i]))
				return false;
		return true;
	}

	const Expr* Emitter::genericSelect(const Expr* e) {
		CType ctrl;
		if(!typeOf(e->generic.control, ctrl))
			return nullptr;
		if(ctrl.array != nullptr && ctrl.ptr == 0)
			ctrl = decay(ctrl);
		clearTopConst(ctrl);

		const Expr* fallback = nullptr;
		for(const GenericAssoc& a : e->assocs) {
			if(a.isDefault) {
				fallback = a.result;
				continue;
			}
			CType at = a.type;
			if(genericTypesMatch(ctrl, at))
				return a.result;
		}
		if(fallback)
			return fallback;
		fail("no _Generic association matches the controlling type");
		return nullptr;
	}

	B32 Emitter::typeOfUnary(const Expr* e, CType& out) {
		switch(e->unary.op) {
		case ExprOp::Not:
			out = ctInt();
			return true;
		case ExprOp::PreInc:
		case ExprOp::PreDec:
		case ExprOp::PostInc:
		case ExprOp::PostDec:
			return typeOf(e->unary.operand, out);
		case ExprOp::Addr: {
			CType t;
			if(!typeOf(e->unary.operand, t))
				return false;
			out = pointerTo(t);
			return true;
		}
		case ExprOp::Deref: {
			CType t;
			if(!typeOf(e->unary.operand, t))
				return false;
			if(t.func && t.ptr == 1) {
				out = t;
				return true;
			}
			if(isArrayType(t))
				t = decay(t);
			if(!isPointer(t)) {
				fail("indirection requires a pointer operand");
				return false;
			}
			out = pointee(t);
			return true;
		}
		case ExprOp::Real:
		case ExprOp::Imag: {
			CType t;
			if(!typeOf(e->unary.operand, t))
				return false;
			out = isComplexType(t) ? complexElem(t) : t;
			return true;
		}
		default: {
			CType t;
			if(!typeOf(e->unary.operand, t))
				return false;
			out = isPointer(t) ? t : promote(t);
			return true;
		}
		}
	}

	B32 Emitter::typeOfBinary(const Expr* e, CType& out) {
		if(isAssignOp(e->binary.op))
			return typeOf(e->binary.lhs, out);
		switch(e->binary.op) {
		case ExprOp::Lt:
		case ExprOp::Gt:
		case ExprOp::Le:
		case ExprOp::Ge:
		case ExprOp::Eq:
		case ExprOp::Ne:
		case ExprOp::LogAnd:
		case ExprOp::LogOr:
			out = ctInt();
			return true;
		case ExprOp::Shl:
		case ExprOp::Shr: {
			CType l;
			if(!typeOf(e->binary.lhs, l))
				return false;
			out = promote(l);
			return true;
		}
		default: {
			CType l, r;
			if(!typeOf(e->binary.lhs, l) || !typeOf(e->binary.rhs, r))
				return false;
			if(isPointer(l) || isPointer(r)) {
				if(e->binary.op == ExprOp::Sub && isPointer(l) && isPointer(r))
					out = ctPtrDiff();
				else
					out = isPointer(l) ? l : r;
				return true;
			}
			out = usualArithmetic(l, r);
			return true;
		}
		}
	}

	B32 Emitter::typeOf(const Expr* e, CType& out) {
		curOffset = e->offset;
		switch(e->kind) {
		case ExprKind::IntLit:
			out.bits = e->intLit.bits;
			out.mods = e->intLit.mods;
			out.base = CType::Base::Int;
			return true;
		case ExprKind::FloatLit:
			out = CType{};
			out.base = CType::Base::Float;
			out.bits = e->floatLit.bits;
			out.set(CType::Complex, e->floatLit.imaginary);
			return true;
		case ExprKind::StrLit:
			out = CType{};
			out.ptr = 1;
			if(e->str.isWide) {
				out.bits = e->str.charSize * 8;
			} else {
				out.bits = 8;
				out.set(CType::PlainChar);
			}
			return true;
		case ExprKind::Ident: {
			Local loc;
			if(lookup(*e->ident.name, loc)) {
				out = loc.isArray ? pointerTo(loc.type) : loc.type;
				return true;
			}
			auto g = globalVars.find(*e->ident.name);
			if(g != globalVars.end()) {
				out = g->second.isArray ? pointerTo(g->second.type) : g->second.type;
				return true;
			}
			auto f = funcs.find(*e->ident.name);
			if(f != funcs.end()) {
				out = funcPtrType(f->second);
				return true;
			}
			failUndeclared(*e->ident.name);
			return false;
		}
		case ExprKind::Call: {
			if(e->call.callee) {
				if(*e->call.callee == "__builtin_expect") {
					out = CType{};
					out.bits = 64;
					return true;
				}
				if(*e->call.callee == "__builtin_constant_p") {
					out = ctInt();
					return true;
				}
				auto found = funcs.find(*e->call.callee);
				if(found != funcs.end()) {
					out = found->second.ret;
					return true;
				}
				Local loc;
				if(lookup(*e->call.callee, loc) && isFuncPtr(loc.type)) {
					out = loc.type.func->ret;
					return true;
				}
				auto g = globalVars.find(*e->call.callee);
				if(g != globalVars.end() && !g->second.isArray && isFuncPtr(g->second.type)) {
					out = g->second.type.func->ret;
					return true;
				}
				out = ctInt();
				return true;
			}
			CType t;
			if(!typeOf(e->call.target, t))
				return false;
			out = isFuncPtr(t) ? t.func->ret : ctInt();
			return true;
		}
		case ExprKind::Cast:
			out = e->cast.type;
			return resolveType(out);
		case ExprKind::Sizeof:
			out = ctSize();
			return true;
		case ExprKind::VaArg:
			out = e->vaArg.type;
			return true;
		case ExprKind::Unary:
			return typeOfUnary(e, out);
		case ExprKind::Binary:
			return typeOfBinary(e, out);
		case ExprKind::Ternary: {
			CType a, b;
			if(!typeOf(e->ternary.whenTrue, a) || !typeOf(e->ternary.whenFalse, b))
				return false;
			out = usualArithmetic(a, b);
			return true;
		}
		case ExprKind::Comma:
			return typeOf(e->comma.rhs, out);
		case ExprKind::Member: {
			CType base;
			if(!typeOf(e->member.base, base))
				return false;
			CType st = e->member.arrow ? pointee(base) : base;
			if(!isStruct(st)) {
				fail("member reference base type is not a struct or union");
				return false;
			}
			const Field* f = st.strukt->find(*e->member.name);
			if(!f) {
				fail("no member named '" + *e->member.name + "' in '" + typeName(st) + "'");
				return false;
			}
			out = f->isArray() ? pointerTo(f->type) : f->type;
			return true;
		}
		case ExprKind::InitList:
			fail("initializer list has no type");
			return false;
		case ExprKind::CompoundLit:
			out = e->compound.isArray ? pointerTo(e->compound.type) : e->compound.type;
			return true;
		case ExprKind::StmtExpr: {
			const List<Stmt*>& stmts = e->stmtExpr.body->body;
			if(!stmts.empty()) {
				const Stmt* last = stmts.back();
				if(last->kind == StmtKind::Expr && last->expr)
					return typeOf(last->expr, out);
			}
			out = CType{};
			out.base = CType::Base::Void;
			return true;
		}
		case ExprKind::Generic: {
			const Expr* sel = genericSelect(e);
			if(!sel)
				return false;
			return typeOf(sel, out);
		}
		}
		return false;
	}
} // namespace rat::cc
