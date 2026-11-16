#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	// ( type-name ) unary | ( type-name ) initializer postfix-tail
	// only a braced initializer starts a compound literal
	Expr* Parser::parseCastOrCompound() {
		Token lp = advance(); // (
		CType ty;
		DeclResult r;
		if(!parseTypeSpec(ty) || !parseAbstractDeclarator(ty, r) || !expect(TokKind::RParen))
			return nullptr;
		if(check(TokKind::LBrace)) {
			Expr* init = parseInitializer();
			if(!init)
				return nullptr;
			Expr* e = makeExpr(ExprKind::CompoundLit, lp.offset);
			e->compound = {r.type, init};
			return parsePostfixTail(e); // allow [..], .x, etc. after the literal
		}
		if(isArrayType(r.type))
			return fail(lp, "array types may not be used in a cast");
		Expr* operand = parseUnary();
		if(!operand)
			return nullptr;
		Expr* e = makeExpr(ExprKind::Cast, lp.offset);
		e->cast = {r.type, operand};
		return e;
	}

	// size-op ( type-name ) | size-op unary
	// size-op: sizeof | _Alignof
	Expr* Parser::parseSizeOp() {
		Token kw = advance(); // sizeof / _Alignof
		ExprKind kind = kw.kind == TokKind::KwSizeof ? ExprKind::Sizeof : ExprKind::AlignOf;
		Expr* e = makeExpr(kind, kw.offset);
		if(check(TokKind::LParen) && startsType(peek2())) {
			advance(); // (
			CType ty;
			if(!parseTypeName(ty) || !expect(TokKind::RParen))
				return nullptr;
			e->sizeOf = {ty, nullptr};
		} else {
			e->sizeOf.operand = parseUnary();
			if(!e->sizeOf.operand)
				return nullptr;
		}
		return e;
	}

	// size-op-expr | cast-or-compound | unary-op unary | ++ unary | -- unary | primary postfix-tail
	Expr* Parser::parseUnary() {
		DepthScope scope(*this);
		if(!enterDepth())
			return nullptr;
		if(check(TokKind::KwSizeof) || check(TokKind::KwAlignof))
			return parseSizeOp();
		if(check(TokKind::LParen) && startsType(peek2()))
			return parseCastOrCompound();
		ExprOp op = detail::tokSyntax(peek().kind).unaryOp;
		if(op != detail::kNoOp) {
			Token t = advance();
			Expr* operand = parseUnary();
			if(!operand)
				return nullptr;
			return makeUnary(t.offset, op, operand);
		}
		Expr* e = parsePrimary();
		if(!e)
			return nullptr;
		return parsePostfixTail(e);
	}

	// unary [ binary-op unary ]...
	// left-associative, an operator below minPrec ends it
	Expr* Parser::parseBinary(I32 minPrec) {
		Expr* lhs = parseUnary();
		if(!lhs)
			return nullptr;
		for(;;) {
			const detail::TokSyntax& syntax = detail::tokSyntax(peek().kind);
			if(syntax.binaryPrec < minPrec)
				break;
			Token opTok = advance();
			Expr* rhs = parseBinary(syntax.binaryPrec + 1); // left-associative
			if(!rhs)
				return nullptr;
			lhs = makeBinary(opTok.offset, syntax.binaryOp, lhs, rhs);
		}
		return lhs;
	}

	// binary [ ? expr : conditional ]
	Expr* Parser::parseConditional() {
		Expr* cond = parseBinary(1);
		if(!cond)
			return nullptr;
		if(!check(TokKind::Question))
			return cond;
		Token q = advance();
		Expr* whenTrue = parseExpression();
		if(!whenTrue || !expect(TokKind::Colon))
			return nullptr;
		Expr* whenFalse = parseConditional();
		if(!whenFalse)
			return nullptr;
		Expr* e = makeExpr(ExprKind::Ternary, q.offset);
		e->ternary = {cond, whenTrue, whenFalse};
		return e;
	}

	// conditional [ assign-op assignment ]
	Expr* Parser::parseAssignment() {
		Expr* lhs = parseConditional();
		if(!lhs)
			return nullptr;
		ExprOp op = detail::tokSyntax(peek().kind).assignOp;
		if(op != detail::kNoOp) {
			Token opTok = advance();
			Expr* rhs = parseAssignment(); // right-associative
			if(!rhs)
				return nullptr;
			return makeBinary(opTok.offset, op, lhs, rhs);
		}
		return lhs;
	}

	// assignment [, assignment]...
	Expr* Parser::parseExpression() {
		Expr* e = parseAssignment();
		if(!e)
			return nullptr;
		while(check(TokKind::Comma)) {
			Token c = advance();
			Expr* rhs = parseAssignment();
			if(!rhs)
				return nullptr;
			Expr* comma = makeExpr(ExprKind::Comma, c.offset);
			comma->comma = {e, rhs};
			e = comma;
		}
		return e;
	}
} // namespace rat::cc
