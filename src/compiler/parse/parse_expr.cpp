#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	namespace detail {
		BinInfo binInfo(TokKind kind) {
			// clang-format off
			static const BinInfo kBin[] = {
					{9, ExprOp::Add},    // Plus
					{9, ExprOp::Sub},    // Minus
					{10, ExprOp::Mul},   // Star
					{10, ExprOp::Div},   // Slash
					{10, ExprOp::Rem},   // Percent
					{0, ExprOp::Add},    // PlusPlus
					{0, ExprOp::Add},    // MinusMinus
					{5, ExprOp::BitAnd}, // Amp
					{3, ExprOp::BitOr},  // Pipe
					{4, ExprOp::BitXor}, // Caret
					{0, ExprOp::Add},    // Tilde
					{0, ExprOp::Add},    // Bang
					{2, ExprOp::LogAnd}, // AmpAmp
					{1, ExprOp::LogOr},  // PipePipe
					{7, ExprOp::Lt},     // Lt
					{7, ExprOp::Gt},     // Gt
					{7, ExprOp::Le},     // Le
					{7, ExprOp::Ge},     // Ge
					{6, ExprOp::Eq},     // EqEq
					{6, ExprOp::Ne},     // BangEq
					{8, ExprOp::Shl},    // Shl
					{8, ExprOp::Shr},    // Shr
			};
			// clang-format on
			static_assert(sizeof(kBin) / sizeof(kBin[0]) == (U32)TokKind::Shr - (U32)TokKind::Plus + 1,
										"kBin must cover Plus..Shr");
			if(kind < TokKind::Plus || kind > TokKind::Shr)
				return {0, ExprOp::Add};
			return kBin[(U32)kind - (U32)TokKind::Plus];
		}

		B32 assignOp(TokKind kind, ExprOp& op) {
			if(kind < TokKind::Assign || kind > TokKind::ShrEq)
				return false;
			// clang-format off
			static const ExprOp kOps[] = {
					ExprOp::Assign,    ExprOp::AddAssign, ExprOp::SubAssign, ExprOp::MulAssign,
					ExprOp::DivAssign, ExprOp::RemAssign, ExprOp::AndAssign, ExprOp::OrAssign,
					ExprOp::XorAssign, ExprOp::ShlAssign, ExprOp::ShrAssign,
			};
			// clang-format on
			static_assert(sizeof(kOps) / sizeof(kOps[0]) ==
												(U32)TokKind::ShrEq - (U32)TokKind::Assign + 1,
										"kOps must cover Assign..ShrEq");
			op = kOps[(U32)kind - (U32)TokKind::Assign];
			return true;
		}

		B32 unaryOp(TokKind kind, ExprOp& op) {
			struct Entry {
				TokKind kind;
				ExprOp op;
			};
			static const Entry kUnary[] = {
					{TokKind::Plus, ExprOp::Pos},
					{TokKind::Minus, ExprOp::Neg},
					{TokKind::Bang, ExprOp::Not},
					{TokKind::Tilde, ExprOp::BitNot},
					{TokKind::Amp, ExprOp::Addr},
					{TokKind::Star, ExprOp::Deref},
					{TokKind::KwReal, ExprOp::Real},
					{TokKind::KwImag, ExprOp::Imag},
					{TokKind::PlusPlus, ExprOp::PreInc},
					{TokKind::MinusMinus, ExprOp::PreDec},
			};
			for(const Entry& e : kUnary)
				if(e.kind == kind) {
					op = e.op;
					return true;
				}
			return false;
		}
	} // namespace detail

	// ( type-name ) unary | ( type-name ) initializer postfix-tail
	// only a braced initializer starts a compound literal
	Expr* Parser::parseCastOrCompound() {
		Token lp = advance(); // (
		CType ty;
		DeclResult r;
		if(!parseTypeSpec(ty) || !parseAbstractDeclarator(ty, r) || !expect(TokKind::RParen, "')'"))
			return nullptr;
		if(check(TokKind::LBrace)) {
			Expr* init = parseInitializer();
			if(!init)
				return nullptr;
			Expr* e = makeExpr(ExprKind::CompoundLit, lp.offset);
			// a typedef'd array type stays whole
			CType litType = r.outerArray ? r.type.array->elem : r.type;
			e->compound = {litType, init, r.outerBound, r.outerArray};
			return parsePostfixTail(e); // allow [..], .x, etc. after the literal
		}
		if(r.outerArray)
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
			if(!parseTypeName(ty) || !expect(TokKind::RParen, "')'"))
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
		ExprOp op;
		if(detail::unaryOp(peek().kind, op)) {
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
			detail::BinInfo info = detail::binInfo(peek().kind);
			if(info.prec == 0 || info.prec < minPrec)
				break;
			Token opTok = advance();
			Expr* rhs = parseBinary(info.prec + 1); // left-associative
			if(!rhs)
				return nullptr;
			lhs = makeBinary(opTok.offset, info.op, lhs, rhs);
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
		if(!whenTrue || !expect(TokKind::Colon, "':'"))
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
		ExprOp op;
		if(detail::assignOp(peek().kind, op)) {
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
