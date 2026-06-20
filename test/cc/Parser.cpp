#include "Parser.h"

namespace rat::cc {
	namespace {
		struct BinInfo {
			I32 prec;
			ExprOp op;
		};

		BinInfo binInfo(TokKind kind) {
			switch (kind) {
			case TokKind::Star:
				return {10, ExprOp::Mul};
			case TokKind::Slash:
				return {10, ExprOp::Div};
			case TokKind::Percent:
				return {10, ExprOp::Rem};
			case TokKind::Plus:
				return {9, ExprOp::Add};
			case TokKind::Minus:
				return {9, ExprOp::Sub};
			case TokKind::Shl:
				return {8, ExprOp::Shl};
			case TokKind::Shr:
				return {8, ExprOp::Shr};
			case TokKind::Lt:
				return {7, ExprOp::Lt};
			case TokKind::Gt:
				return {7, ExprOp::Gt};
			case TokKind::Le:
				return {7, ExprOp::Le};
			case TokKind::Ge:
				return {7, ExprOp::Ge};
			case TokKind::EqEq:
				return {6, ExprOp::Eq};
			case TokKind::BangEq:
				return {6, ExprOp::Ne};
			case TokKind::Amp:
				return {5, ExprOp::BitAnd};
			case TokKind::Caret:
				return {4, ExprOp::BitXor};
			case TokKind::Pipe:
				return {3, ExprOp::BitOr};
			case TokKind::AmpAmp:
				return {2, ExprOp::LogAnd};
			case TokKind::PipePipe:
				return {1, ExprOp::LogOr};
			default:
				return {0, ExprOp::Add};
			}
		}

		B32 assignOp(TokKind kind, ExprOp& op) {
			switch (kind) {
			case TokKind::Assign:
				op = ExprOp::Assign;
				return true;
			case TokKind::PlusEq:
				op = ExprOp::AddAssign;
				return true;
			case TokKind::MinusEq:
				op = ExprOp::SubAssign;
				return true;
			case TokKind::StarEq:
				op = ExprOp::MulAssign;
				return true;
			case TokKind::SlashEq:
				op = ExprOp::DivAssign;
				return true;
			case TokKind::PercentEq:
				op = ExprOp::RemAssign;
				return true;
			case TokKind::ShlEq:
				op = ExprOp::ShlAssign;
				return true;
			case TokKind::ShrEq:
				op = ExprOp::ShrAssign;
				return true;
			case TokKind::AmpEq:
				op = ExprOp::AndAssign;
				return true;
			case TokKind::PipeEq:
				op = ExprOp::OrAssign;
				return true;
			case TokKind::CaretEq:
				op = ExprOp::XorAssign;
				return true;
			default:
				return false;
			}
		}

		B32 unaryOp(TokKind kind, ExprOp& op) {
			switch (kind) {
			case TokKind::Plus:
				op = ExprOp::Pos;
				return true;
			case TokKind::Minus:
				op = ExprOp::Neg;
				return true;
			case TokKind::Bang:
				op = ExprOp::Not;
				return true;
			case TokKind::Tilde:
				op = ExprOp::BitNot;
				return true;
			default:
				return false;
			}
		}
	} // namespace

	Parser::Parser(Lexer& lexer, Arena& arena) : lex(lexer), arena(arena) {}

	void Parser::fail(const Token& at, const String& msg) {
		if (failed)
			return;
		std::ostringstream os;
		os << lex.file() << ":" << at.line << ":" << at.col << ": " << msg;
		errMsg = os.str();
		failed = true;
	}

	B32 Parser::accept(TokKind kind) {
		if (peek().kind == kind) {
			advance();
			return true;
		}
		return false;
	}

	B32 Parser::expect(TokKind kind, const char* what) {
		if (peek().kind == kind) {
			advance();
			return true;
		}
		fail(peek(), String("expected ") + what + ", found '" +
										 tokKindName(peek().kind) + "'");
		return false;
	}

	Expr* Parser::makeExpr(ExprKind kind, U32 offset) {
		Expr* e = arena.make<Expr>();
		e->kind = kind;
		e->offset = offset;
		return e;
	}

	Expr* Parser::makeInt(const Token& tok, I64 value, B32 isUnsigned,
												B32 isLong) {
		Expr* e = makeExpr(ExprKind::IntLit, tok.offset);
		e->intLit = {value, isUnsigned, isLong};
		return e;
	}

	Expr* Parser::makeIdent(const Token& tok) {
		Expr* e = makeExpr(ExprKind::Ident, tok.offset);
		e->ident.name = arena.make<String>(lex.text(tok));
		return e;
	}

	Expr* Parser::makeUnary(U32 offset, ExprOp op, Expr* operand) {
		Expr* e = makeExpr(ExprKind::Unary, offset);
		e->unary = {op, operand};
		return e;
	}

	Expr* Parser::makeBinary(U32 offset, ExprOp op, Expr* lhs, Expr* rhs) {
		Expr* e = makeExpr(ExprKind::Binary, offset);
		e->binary = {op, lhs, rhs};
		return e;
	}

	B32 Parser::parseIntLiteral(const Token& tok, I64& value, B32& isUnsigned,
															B32& isLong) {
		String s = lex.text(tok);
		isUnsigned = false;
		isLong = false;

		U32 end = (U32)s.size();
		while (end > 0) {
			char c = s[end - 1];
			if (c == 'u' || c == 'U') {
				isUnsigned = true;
				--end;
			} else if (c == 'l' || c == 'L') {
				isLong = true;
				--end;
			} else {
				break;
			}
		}

		I32 base = 10;
		U32 start = 0;
		if (end >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			start = 2;
		} else if (end >= 1 && s[0] == '0') {
			base = 8;
		}

		U64 v = 0;
		for (U32 i = start; i < end; ++i) {
			char c = s[i];
			I32 d;
			if (c >= '0' && c <= '9')
				d = c - '0';
			else if (c >= 'a' && c <= 'f')
				d = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				d = c - 'A' + 10;
			else {
				fail(tok, "invalid digit in integer constant");
				return false;
			}
			if (d >= base) {
				fail(tok, "invalid digit in integer constant");
				return false;
			}
			v = v * (U64)base + (U64)d;
		}

		value = (I64)v;
		return true;
	}

	Expr* Parser::parsePrimary() {
		const Token& tok = peek();
		if (tok.kind == TokKind::IntConstant) {
			Token lit = advance();
			I64 value;
			B32 isUnsigned, isLong;
			if (!parseIntLiteral(lit, value, isUnsigned, isLong))
				return nullptr;
			return makeInt(lit, value, isUnsigned, isLong);
		}
		if (tok.kind == TokKind::Identifier) {
			Token id = advance();
			return makeIdent(id);
		}
		if (tok.kind == TokKind::LParen) {
			advance();
			Expr* inner = parseExpression();
			if (!inner)
				return nullptr;
			if (!expect(TokKind::RParen, "')'"))
				return nullptr;
			return inner;
		}
		fail(tok, String("expected expression, found '") + tokKindName(tok.kind) +
								 "'");
		return nullptr;
	}

	Expr* Parser::parseUnary() {
		ExprOp op;
		if (unaryOp(peek().kind, op)) {
			Token t = advance();
			Expr* operand = parseUnary();
			if (!operand)
				return nullptr;
			return makeUnary(t.offset, op, operand);
		}
		return parsePrimary();
	}

	Expr* Parser::parseBinary(I32 minPrec) {
		Expr* lhs = parseUnary();
		if (!lhs)
			return nullptr;
		for (;;) {
			BinInfo info = binInfo(peek().kind);
			if (info.prec == 0 || info.prec < minPrec)
				break;
			Token opTok = advance();
			Expr* rhs = parseBinary(info.prec + 1); // left-associative
			if (!rhs)
				return nullptr;
			lhs = makeBinary(opTok.offset, info.op, lhs, rhs);
		}
		return lhs;
	}

	Expr* Parser::parseConditional() {
		Expr* cond = parseBinary(1);
		if (!cond)
			return nullptr;
		if (peek().kind != TokKind::Question)
			return cond;
		Token q = advance();
		Expr* whenTrue = parseExpression();
		if (!whenTrue)
			return nullptr;
		if (!expect(TokKind::Colon, "':'"))
			return nullptr;
		Expr* whenFalse = parseConditional();
		if (!whenFalse)
			return nullptr;
		Expr* e = makeExpr(ExprKind::Ternary, q.offset);
		e->ternary = {cond, whenTrue, whenFalse};
		return e;
	}

	Expr* Parser::parseAssignment() {
		Expr* lhs = parseConditional();
		if (!lhs)
			return nullptr;
		ExprOp op;
		if (assignOp(peek().kind, op)) {
			Token opTok = advance();
			Expr* rhs = parseAssignment(); // right-associative
			if (!rhs)
				return nullptr;
			return makeBinary(opTok.offset, op, lhs, rhs);
		}
		return lhs;
	}

	Expr* Parser::parseExpression() {
		Expr* e = parseAssignment();
		if (!e)
			return nullptr;
		while (peek().kind == TokKind::Comma) {
			Token c = advance();
			Expr* rhs = parseAssignment();
			if (!rhs)
				return nullptr;
			Expr* comma = makeExpr(ExprKind::Comma, c.offset);
			comma->comma = {e, rhs};
			e = comma;
		}
		return e;
	}

	Stmt* Parser::parseDeclaration() {
		Token start = peek();
		if (!expect(TokKind::KwInt, "'int'"))
			return nullptr;
		Stmt* s = arena.make<Stmt>();
		s->kind = StmtKind::Decl;
		s->offset = start.offset;
		for (;;) {
			if (peek().kind != TokKind::Identifier) {
				fail(peek(), "expected declarator name");
				return nullptr;
			}
			Token nameTok = advance();
			Declarator d;
			d.name = arena.make<String>(lex.text(nameTok));
			d.offset = nameTok.offset;
			if (accept(TokKind::Assign)) {
				d.init = parseAssignment();
				if (!d.init)
					return nullptr;
			}
			s->decls.push_back(d);
			if (!accept(TokKind::Comma))
				break;
		}
		if (!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	Stmt* Parser::parseStatement() {
		const Token& tok = peek();
		if (tok.kind == TokKind::LBrace)
			return parseCompound();

		if (tok.kind == TokKind::KwInt)
			return parseDeclaration();

		if (tok.kind == TokKind::KwReturn) {
			Token kw = advance();
			Stmt* s = arena.make<Stmt>();
			s->kind = StmtKind::Return;
			s->offset = kw.offset;
			if (peek().kind != TokKind::Semicolon) {
				s->expr = parseExpression();
				if (!s->expr)
					return nullptr;
			}
			if (!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if (tok.kind == TokKind::Semicolon) {
			Token semi = advance();
			Stmt* s = arena.make<Stmt>();
			s->kind = StmtKind::Empty;
			s->offset = semi.offset;
			return s;
		}

		Stmt* s = arena.make<Stmt>();
		s->kind = StmtKind::Expr;
		s->offset = tok.offset;
		s->expr = parseExpression();
		if (!s->expr)
			return nullptr;
		if (!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	Stmt* Parser::parseCompound() {
		Token open = peek();
		if (!expect(TokKind::LBrace, "'{'"))
			return nullptr;
		Stmt* block = arena.make<Stmt>();
		block->kind = StmtKind::Compound;
		block->offset = open.offset;
		while (!failed && peek().kind != TokKind::RBrace &&
					 peek().kind != TokKind::Eof) {
			Stmt* s = parseStatement();
			if (!s)
				return nullptr;
			block->body.push_back(s);
		}
		if (!expect(TokKind::RBrace, "'}'"))
			return nullptr;
		return block;
	}

	FuncDef* Parser::parseFunction() {
		Token start = peek();
		if (!expect(TokKind::KwInt, "'int'"))
			return nullptr;
		if (peek().kind != TokKind::Identifier) {
			fail(peek(), "expected function name");
			return nullptr;
		}
		Token nameTok = advance();
		if (!expect(TokKind::LParen, "'('"))
			return nullptr;
		if (peek().kind == TokKind::KwVoid)
			advance();
		if (!expect(TokKind::RParen, "')'"))
			return nullptr;

		FuncDef* fn = arena.make<FuncDef>();
		fn->name = lex.text(nameTok);
		fn->retType = TypeSpec::Int;
		fn->offset = start.offset;
		fn->body = parseCompound();
		if (!fn->body)
			return nullptr;
		return fn;
	}

	TransUnit* Parser::parseUnit() {
		TransUnit* unit = arena.make<TransUnit>();
		while (!failed && peek().kind != TokKind::Eof) {
			FuncDef* fn = parseFunction();
			if (!fn)
				return nullptr;
			unit->functions.push_back(fn);
		}
		if (failed)
			return nullptr;
		return unit;
	}
} // namespace rat::cc
