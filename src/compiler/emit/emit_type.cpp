#include "emit/emit.h"

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

	B32 Emitter::identIsArray(const String& name) {
		Local loc;
		if(func.scopes.lookup(name, loc))
			return loc.isArray;
		auto gv = syms.globals.find(name);
		return gv != syms.globals.end() && gv->second.isArray;
	}

	B32 Emitter::sizeofOperand(const Expr* operand, U64& out) {
		if(operand->kind == ExprKind::StrLit) {
			out = (U32)operand->str.bytes->size() + operand->str.charSize;
			return true;
		}
		if(operand->kind == ExprKind::Ident) {
			const String& name = *operand->ident.name;
			Local loc;
			B32 isLocal = func.scopes.lookup(name, loc);
			if(isLocal && loc.isArray) {
				if(loc.lengthNode)
					return false;
				out = loc.count * byteSize(loc.type);
				return true;
			}
			auto gv = syms.globals.find(name);
			if(gv != syms.globals.end()) {
				if(gv->second.isArray) {
					out = gv->second.count * byteSize(gv->second.type);
					return true;
				}
			} else if(!isLocal && syms.functions.count(name)) {
				out = 1;
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
				if(f && isArrayType(f->type)) {
					out = byteSize(f->type);
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
		if(a.ptr != b.ptr || a.quals != b.quals || (a.func != nullptr) != (b.func != nullptr))
			return false;
		if(a.func && b.func)
			return funcTypesMatch(a.func, b.func);
		B32 aArr = a.array != nullptr && a.ptr == 0;
		B32 bArr = b.array != nullptr && b.ptr == 0;
		if(aArr != bArr)
			return false;
		if(aArr && bArr)
			return a.array->count == b.array->count && genericTypesMatch(a.array->elem, b.array->elem);
		if((a.strukt != nullptr) != (b.strukt != nullptr))
			return false;
		if(a.strukt && b.strukt)
			return a.strukt == b.strukt;
		if(a.isVoid() != b.isVoid() || a.isFloat() != b.isFloat() || a.bits != b.bits)
			return false;
		if(a.isFloat() || a.isVoid())
			return true;
		return a.isUnsigned() == b.isUnsigned() &&
					 !(a.bits == 8 && a.isPlainChar() != b.isPlainChar()) &&
					 !(a.bits == 32 && a.isLong() != b.isLong()) &&
					 !(a.bits == 64 && a.isLongLong() != b.isLongLong());
	}

	static B32 funcTypesMatch(const FuncType* a, const FuncType* b) {
		if(a->isVarArgs != b->isVarArgs || a->params.size() != b->params.size() ||
			 !genericTypesMatch(a->ret, b->ret))
			return false;
		for(U32 i = 0; i < a->params.size(); ++i)
			if(!genericTypesMatch(a->params[i].type, b->params[i].type))
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
			if(genericTypesMatch(ctrl, a.type))
				return a.result;
		}
		if(fallback)
			return fallback;
		diag.fail("no _Generic association matches the controlling type");
		return nullptr;
	}

	B32 Emitter::typeOfUnary(const Expr* e, CType& out) {
		if(e->unary.op == ExprOp::Not) {
			out = ctInt();
			return true;
		}
		const Expr* operand = e->unary.operand;
		CType t;
		if(!typeOf(operand, t))
			return false;
		switch(e->unary.op) {
		case ExprOp::PreInc:
		case ExprOp::PreDec:
		case ExprOp::PostInc:
		case ExprOp::PostDec:
			out = t;
			return true;
		case ExprOp::Addr:
			if(operand->kind == ExprKind::Ident && identIsArray(*operand->ident.name))
				out = t;
			else
				out = pointerTo(t);
			return true;
		case ExprOp::Deref:
			if(t.func && t.ptr == 1) {
				out = t;
				return true;
			}
			if(isArrayType(t))
				t = decay(t);
			if(!isPointer(t)) {
				diag.fail("indirection requires a pointer operand");
				return false;
			}
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
			Local loc;
			if(func.scopes.lookup(*e->ident.name, loc)) {
				out = loc.isArray ? pointerTo(loc.type) : loc.type;
				return true;
			}
			auto g = syms.globals.find(*e->ident.name);
			if(g != syms.globals.end()) {
				out = g->second.isArray ? pointerTo(g->second.type) : g->second.type;
				return true;
			}
			auto f = syms.functions.find(*e->ident.name);
			if(f != syms.functions.end()) {
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
				auto found = syms.functions.find(*e->call.callee);
				if(found != syms.functions.end()) {
					out = found->second.returnType;
					return true;
				}
				if(builtinReturnType(*e->call.callee, lay.longBits, out))
					return true;
				Local loc;
				if(func.scopes.lookup(*e->call.callee, loc) && isFuncPtr(loc.type)) {
					out = loc.type.func->ret;
					return true;
				}
				auto g = syms.globals.find(*e->call.callee);
				if(g != syms.globals.end() && !g->second.isArray && isFuncPtr(g->second.type)) {
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
			CType base;
			if(!typeOf(e->member.base, base))
				return false;
			CType st = e->member.arrow ? pointee(base) : base;
			if(!isStruct(st)) {
				diag.fail("member reference base type is not a struct or union");
				return false;
			}
			const Field* f = st.strukt->find(*e->member.name);
			if(!f) {
				diag.fail("no member named '" + *e->member.name + "' in '" + typeName(st) + "'");
				return false;
			}
			if(f->isArray())
				out = pointerTo(f->type);
			else
				out = isArrayType(f->type) ? decay(f->type) : f->type;
			return true;
		}
		case ExprKind::InitList:
			diag.fail("initializer list has no type");
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
