#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	// an array of the code units with the terminator
	CType Parser::strType(const Expr* e) {
		CType elem;
		elem.bits = e->str.charSize * 8;
		elem.set(CType::PlainChar, !e->str.isWide);
		U64 units = e->str.bytes->size() / e->str.charSize + 1;
		CType t;
		t.array = arena.make<ArrayType>(ArrayType{elem, units});
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
		const Ident* id = idents.get(name);
		B32 declared = id && id->kind == Ident::Kind::Object;
		// see emitAbsBuiltin
		B32 handledAsBuiltin = !declared || !detail::isAbsName(name) || integerArg;
		if((handledAsBuiltin && builtinReturnType(name, lay.longBits, out)) || !declared)
			return out;
		CType t = valueType(id->type);
		return isFuncPtr(t) ? t.func->ret : out;
	}

	CType Parser::unaryType(const Expr* e) {
		CType t = e->unary.operand->type;
		CType v = valueType(t);
		switch(e->unary.op) {
		case ExprOp::PreInc:
		case ExprOp::PreDec:
		case ExprOp::PostInc:
		case ExprOp::PostDec:
			return rvalueType(t);
		case ExprOp::Addr:
			return pointerTo(t);
		case ExprOp::Deref:
			if(!isPointer(v))
				return fail(peek(), "indirection requires a pointer operand");
			return pointee(v); // a function pointer gives the function
		case ExprOp::Not:
			return ctInt();
		case ExprOp::Real:
		case ExprOp::Imag:
			return isComplexType(v) ? complexElem(v) : v;
		default:
			return promote(rvalueType(t)); // a floating or complex operand keeps its type
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
			return ptrDiffType(lay);
		if(isPointer(l))
			return l;
		if(isPointer(r))
			return r;
		return usualArithmetic(l, r);
	}

	// a volatile struct has volatile members
	CType Parser::memberType(const Expr* e) {
		CType base = valueType(e->member.base->type);
		if(e->member.arrow && !isPointer(base))
			return fail(peek(), "'->' requires a pointer to struct or union");
		CType st = e->member.arrow ? pointee(base) : base;
		if(!isStruct(st))
			return fail(peek(), "member reference base type is not a struct or union");
		const String& name = *e->member.name;
		const Field* f = st.strukt->find(name);
		if(!f)
			return fail(peek(), "no member named '" + name + "' in '" + typeName(st) + "'");
		CType type = f->type;
		if(isTopVolatile(st))
			setTopVolatile(type);
		return type;
	}

	// the type of e from its children, which are typed already
	CType Parser::typeOf(const Expr* e) {
		switch(e->kind) {
		case ExprKind::IntLit:
			return {e->intLit.bits, 0, 0, 0, 0, CType::Base::Int, e->intLit.mods};
		case ExprKind::FloatLit:
			return ctFloat(e->floatLit.bits, e->floatLit.imaginary);
		case ExprKind::StrLit:
			return strType(e);
		case ExprKind::Call:
			return callType(e);
		case ExprKind::Cast:
			return e->cast.type;
		case ExprKind::Sizeof:
		case ExprKind::AlignOf:
			return sizeType(lay);
		case ExprKind::Unary:
			return unaryType(e);
		case ExprKind::Binary:
			return binaryType(e);
		case ExprKind::Ternary:
			return ternaryType(e->ternary.whenTrue, e->ternary.whenFalse);
		case ExprKind::Comma:
			return rvalueType(e->comma.rhs->type);
		case ExprKind::Member:
			return memberType(e);
		case ExprKind::CompoundLit:
			return e->compound.type;
		case ExprKind::VaArg:
			return e->vaArg.type;
		case ExprKind::StmtExpr:
			return stmtExprType(e);
		case ExprKind::Ident:
		case ExprKind::InitList:
			break; // an ident is typed from its declaration, see parsePrimaryName
		}
		return e->type;
	}

	// sets e->type, null after a diagnostic
	Expr* Parser::typed(Expr* e) {
		e->type = typeOf(e);
		return failed ? nullptr : e;
	}
} // namespace rat::cc
