#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	CType Parser::sizeType() const { return ctUnsigned(lay.ptrBytes * 8); }

	CType Parser::ptrDiffType() const { return {lay.ptrBytes * 8}; }

	// an array of the code units with the terminator
	CType Parser::strType(const Expr* e) {
		CType elem;
		elem.bits = e->str.charSize * 8;
		elem.set(CType::PlainChar, !e->str.isWide);
		ArrayType* at = arena.make<ArrayType>();
		at->elem = elem;
		at->count = e->str.bytes->size() / e->str.charSize + 1;
		CType t;
		t.array = at;
		return t;
	}

	// the value of the last expression statement, void without one
	CType Parser::stmtExprType(const Expr* e) {
		const List<Stmt*>& stmts = e->stmtExpr.body->body;
		if(stmts.empty() || stmts.back()->kind != StmtKind::Expr || !stmts.back()->expr)
			return ctVoid();
		return rvalueType(stmts.back()->expr->type);
	}

	namespace detail {
		// abs, labs or llabs, with or without the __builtin_ prefix
		B32 isAbsName(const String& name) {
			U32 start = 0;
			if(name.rfind("__builtin_", 0) == 0)
				start = 10;
			const C8* stem = name.c_str() + start;
			return !strcmp(stem, "abs") || !strcmp(stem, "labs") || !strcmp(stem, "llabs");
		}
	} // namespace detail

	// a builtin first, then the callee's own type, then an implicit declaration, see emitCall
	CType Parser::callType(const Expr* e) {
		CType out = ctInt();
		if(e->call.target) {
			CType t = valueType(e->call.target->type);
			return isFuncPtr(t) ? t.func->ret : out;
		}
		const String& name = *e->call.callee;
		B32 integerArg = e->args.size() == 1 && isInteger(valueType(e->args[0]->type));
		B32 handledAsBuiltin = !detail::isAbsName(name) || integerArg; // see emitAbsBuiltin
		if(handledAsBuiltin && builtinReturnType(name, lay.longBits, out))
			return out;
		const Ident* id = idents.get(name);
		if(!id || id->kind != Ident::Kind::Object) {
			builtinReturnType(name, lay.longBits, out);
			return out;
		}
		CType t = valueType(id->type);
		return isFuncPtr(t) ? t.func->ret : out;
	}

	B32 Parser::unaryType(Expr* e) {
		CType t = e->unary.operand->type;
		CType v = valueType(t);
		switch(e->unary.op) {
		case ExprOp::PreInc:
		case ExprOp::PreDec:
		case ExprOp::PostInc:
		case ExprOp::PostDec:
			e->type = rvalueType(t);
			return true;
		case ExprOp::Addr:
			e->type = pointerTo(t);
			return true;
		case ExprOp::Deref:
			if(!isPointer(v))
				return fail(peek(), "indirection requires a pointer operand");
			e->type = pointee(v); // a function pointer gives the function
			return true;
		case ExprOp::Not:
			e->type = ctInt();
			return true;
		case ExprOp::Real:
		case ExprOp::Imag:
			e->type = isComplexType(v) ? complexElem(v) : v;
			return true;
		default: {
			CType r = rvalueType(t);
			e->type = isComplexType(r) ? r : promote(r);
			return true;
		}
			return true;
		}
	}

	CType Parser::binaryType(const Expr* e) {
		ExprOp op = e->binary.op;
		if(isAssignOp(op))
			return rvalueType(e->binary.lhs->type);
		B32 isCompare = op >= ExprOp::Lt && op <= ExprOp::Ne;
		if(isCompare || op == ExprOp::LogAnd || op == ExprOp::LogOr)
			return ctInt();
		CType l = rvalueType(e->binary.lhs->type);
		CType r = rvalueType(e->binary.rhs->type);
		if(isComplexType(l) || isComplexType(r))
			return usualArithmetic(l, r);
		if(op == ExprOp::Shl || op == ExprOp::Shr)
			return promote(l);
		if(op == ExprOp::Sub && isPointer(l) && isPointer(r))
			return ptrDiffType();
		if(isPointer(l))
			return l;
		if(isPointer(r))
			return r;
		return usualArithmetic(l, r);
	}

	// a volatile struct has volatile members
	B32 Parser::memberType(Expr* e) {
		CType base = valueType(e->member.base->type);
		CType st = e->member.arrow ? pointee(base) : base;
		if(!isStruct(st))
			return fail(peek(), "member reference base type is not a struct or union");
		const String& name = *e->member.name;
		const Field* f = st.strukt->find(name);
		if(!f)
			return fail(peek(), "no member named '" + name + "' in '" + typeName(st) + "'");
		e->type = f->type;
		if(isTopVolatile(st))
			setTopVolatile(e->type);
		return true;
	}

	// sets e->type from its children, which are typed already
	// null after a diagnostic
	Expr* Parser::typed(Expr* e) {
		switch(e->kind) {
		case ExprKind::IntLit:
			e->type.bits = e->intLit.bits;
			e->type.mods = e->intLit.mods;
			break;
		case ExprKind::FloatLit:
			e->type = {e->floatLit.bits, 0, 0, 0, 0, CType::Base::Float};
			if(e->floatLit.imaginary) {
				e->type.set(CType::Complex);
				e->type.strukt = complexLayout(e->type.bits);
			}
			break;
		case ExprKind::StrLit:
			e->type = strType(e);
			break;
		case ExprKind::Call:
			e->type = callType(e);
			break;
		case ExprKind::Cast:
			e->type = e->cast.type;
			break;
		case ExprKind::Sizeof:
		case ExprKind::AlignOf:
			e->type = sizeType();
			break;
		case ExprKind::Unary:
			return unaryType(e) ? e : nullptr;
		case ExprKind::Binary:
			e->type = binaryType(e);
			break;
		case ExprKind::Ternary:
			e->type = ternaryType(e->ternary.whenTrue, e->ternary.whenFalse);
			break;
		case ExprKind::Comma:
			e->type = rvalueType(e->comma.rhs->type);
			break;
		case ExprKind::Member:
			return memberType(e) ? e : nullptr;
		case ExprKind::CompoundLit:
			e->type = e->compound.type;
			break;
		case ExprKind::VaArg:
			e->type = e->vaArg.type;
			break;
		case ExprKind::StmtExpr:
			e->type = stmtExprType(e);
			break;
		case ExprKind::Ident:
		case ExprKind::InitList:
			break; // an ident is typed from its declaration, see parsePrimaryName
		}
		return e;
	}
} // namespace rat::cc
