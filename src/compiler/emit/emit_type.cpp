#include "emit/emit.h"

namespace rat::cc {
	B32 Emitter::resolveType(CType& t) {
		if(!t.typeofExpr)
			return true;
		U32 quals = t.quals;
		U32 vquals = t.vquals;
		const Expr* operand = t.typeofExpr;
		if(!typeOf(operand, t))
			return false;
		t.quals |= quals;
		t.vquals |= vquals;
		t.typeofExpr = nullptr;
		return true;
	}

	Symbol Emitter::lookupSymbol(const String& name) {
		Symbol sym;
		const Local* local = func.scopes.get(name);
		if(local && local->kind != Local::Kind::Function) {
			sym.kind = Symbol::Kind::Local;
			sym.local = local;
			sym.type = local->type;
			return sym;
		}
		auto global = syms.globals.find(name);
		if(!local && global != syms.globals.end()) {
			sym.kind = Symbol::Kind::Global;
			sym.type = global->second.type;
			return sym;
		}
		auto function = syms.functions.find(name);
		if(function != syms.functions.end()) {
			sym.kind = Symbol::Kind::Function;
			sym.type = funcPtrType(function->second);
		}
		return sym;
	}

	// false when the base has no type, field is null when there is no such member
	B32 Emitter::memberField(const Expr* e, CType& structType, const Field*& field) {
		field = nullptr;
		CType base;
		if(!typeOf(e->member.base, base))
			return false;
		structType = e->member.arrow ? pointee(base) : base;
		if(structType.strukt)
			field = structType.strukt->find(*e->member.name);
		return true;
	}

	B32 Emitter::sizeofOperand(const Expr* operand, U64& out) {
		if(operand->kind == ExprKind::StrLit) {
			out = (U32)operand->str.bytes->size() + operand->str.charSize;
			return true;
		}
		if(operand->kind == ExprKind::Ident) {
			Symbol sym = lookupSymbol(*operand->ident.name);
			if(sym.local && sym.local->lengthNode)
				return false;
			if(sym.kind == Symbol::Kind::Function) {
				out = 1;
				return true;
			}
		}
		if(operand->kind == ExprKind::CompoundLit && isArrayType(operand->compound.type)) {
			I64 count;
			if(!compoundArrayCount(operand, count))
				return failArrayCount();
			out = (U64)count * byteSize(arrayElem(operand->compound.type));
			return true;
		}
		CType t;
		if(!typeOfObject(operand, t))
			return false;
		out = byteSize(t);
		return true;
	}

	namespace detail {
		B32 genericTypesMatch(const CType& a, const CType& b) {
			if(a.ptr != b.ptr || a.quals != b.quals || a.vquals != b.vquals)
				return false;
			if((a.func != nullptr) != (b.func != nullptr))
				return false;
			if(a.func)
				return funcTypesMatch(a.func, b.func);
			if(isArrayType(a) != isArrayType(b))
				return false;
			if(isArrayType(a))
				return a.array->count == b.array->count && genericTypesMatch(a.array->elem, b.array->elem);
			if((a.strukt != nullptr) != (b.strukt != nullptr))
				return false;
			if(a.strukt)
				return a.strukt == b.strukt;
			if(a.isVoid() != b.isVoid() || a.isFloat() != b.isFloat() || a.bits != b.bits)
				return false;
			if(a.isFloat() || a.isVoid())
				return true;
			if(a.isUnsigned() != b.isUnsigned())
				return false;
			if(a.bits == 8 && a.isPlainChar() != b.isPlainChar())
				return false;
			if(a.bits == 32 && a.isLong() != b.isLong())
				return false;
			return a.bits != 64 || a.isLongLong() == b.isLongLong();
		}

		B32 funcTypesMatch(const FuncType* a, const FuncType* b) {
			if(a->isVarArgs != b->isVarArgs || a->params.size() != b->params.size())
				return false;
			if(!genericTypesMatch(a->ret, b->ret))
				return false;
			for(U32 i = 0; i < a->params.size(); ++i)
				if(!genericTypesMatch(a->params[i].type, b->params[i].type))
					return false;
			return true;
		}
	} // namespace detail

	const Expr* Emitter::genericSelect(const Expr* e) {
		CType ctrl;
		if(!typeOf(e->generic.control, ctrl))
			return nullptr;
		clearTopConst(ctrl);
		clearTopVolatile(ctrl);

		const Expr* fallback = nullptr;
		for(const GenericAssoc& a : e->assocs) {
			if(a.isDefault) {
				fallback = a.result;
				continue;
			}
			if(detail::genericTypesMatch(ctrl, a.type))
				return a.result;
		}
		if(fallback)
			return fallback;
		return diag.fail("no _Generic association matches the controlling type");
	}

	B32 Emitter::typeOfUnary(const Expr* e, CType& out) {
		if(e->unary.op == ExprOp::Not) {
			out = ctInt();
			return true;
		}
		const Expr* operand = e->unary.operand;
		CType t;
		if(e->unary.op == ExprOp::Addr) {
			if(!typeOfObject(operand, t))
				return false;
		} else if(!typeOf(operand, t)) {
			return false;
		}
		switch(e->unary.op) {
		case ExprOp::PreInc:
		case ExprOp::PreDec:
		case ExprOp::PostInc:
		case ExprOp::PostDec:
			out = t;
			return true;
		case ExprOp::Addr:
			out = pointerTo(t);
			return true;
		case ExprOp::Deref:
			if(t.func && t.ptr == 1) {
				out = t;
				return true;
			}
			if(!isPointer(t))
				return diag.fail("indirection requires a pointer operand");
			out = pointee(t);
			return true;
		case ExprOp::Real:
		case ExprOp::Imag:
			out = isComplexType(t) ? complexElem(t) : t;
			return true;
		default:
			out = isPointer(t) ? t : promote(t);
			return true;
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
		if(!typeOfObject(e, out))
			return false;
		if(isArrayType(out))
			out = decay(out);
		return true;
	}

	// an array keeps its type
	B32 Emitter::typeOfObject(const Expr* e, CType& out) {
		diag.offset = e->offset;
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
			Symbol sym = lookupSymbol(*e->ident.name);
			if(sym.kind == Symbol::Kind::None)
				return failUndeclared(*e->ident.name);
			out = sym.type;
			return true;
		}
		case ExprKind::Call: {
			if(e->call.callee) {
				Symbol sym = lookupSymbol(*e->call.callee);
				if(isFuncPtr(sym.type)) {
					out = sym.type.func->ret;
					return true;
				}
				out = ctInt();
				builtinReturnType(*e->call.callee, lay.longBits, out);
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
		case ExprKind::AlignOf:
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
			CType st;
			const Field* f;
			if(!memberField(e, st, f))
				return false;
			if(!isStruct(st))
				return diag.fail("member reference base type is not a struct or union");
			if(!f)
				return diag.fail("no member named '" + *e->member.name + "' in '" + typeName(st) + "'");
			out = f->type;
			return true;
		}
		case ExprKind::InitList:
			return diag.fail("initializer list has no type");
		case ExprKind::CompoundLit:
			out = e->compound.type;
			return true;
		case ExprKind::StmtExpr: {
			const List<Stmt*>& stmts = e->stmtExpr.body->body;
			if(!stmts.empty()) {
				const Stmt* last = stmts.back();
				if(last->kind == StmtKind::Expr && last->expr)
					return typeOf(last->expr, out);
			}
			out = ctVoid();
			return true;
		}
		case ExprKind::Generic: {
			const Expr* sel = genericSelect(e);
			if(!sel)
				return false;
			return typeOfObject(sel, out);
		}
		}
		return false;
	}
} // namespace rat::cc
