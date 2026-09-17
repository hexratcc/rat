#include "parse/parser.h"

namespace rat::cc {
	// _Static_assert ( const-expr , string-literal... ) ;
	B32 Parser::parseStaticAssert() {
		Token kw = advance(); // _Static_assert
		if(!expect(TokKind::LParen, "'('"))
			return false;
		Expr* cond = parseConditional();
		if(!cond)
			return false;
		if(!expect(TokKind::Comma, "','"))
			return false;
		String msg;
		if(!check(TokKind::StringLiteral)) {
			fail(peek(), "expected a string literal in _Static_assert");
			return false;
		}
		if(!parseStringLiteral(advance(), msg))
			return false;
		while(check(TokKind::StringLiteral)) {
			String more;
			if(!parseStringLiteral(advance(), more))
				return false;
			msg += more;
		}
		if(!expect(TokKind::RParen, "')'"))
			return false;
		if(!expect(TokKind::Semicolon, "';'"))
			return false;
		I64 v = 0;
		if(!evalIntConst(cond, v))
			return false;
		if(v == 0) {
			fail(kw, "static assertion failed: " + msg);
			return false;
		}
		return true;
	}

	// a non-extern object must not have an incomplete struct type, through arrays
	B32 Parser::checkObjectComplete(const Declarator& d) {
		if(d.isExtern)
			return true;
		CType ult = d.type;
		while(ult.array != nullptr && ult.ptr == 0)
			ult = ult.array->elem;
		if(isStruct(ult) && (ult.strukt == nullptr || !ult.strukt->complete)) {
			fail(peek(), "variable has incomplete type");
			return false;
		}
		return true;
	}

	// static-assert | typedef | type-spec ; | type-spec declarators
	Stmt* Parser::parseDeclaration() {
		Token start = peek();
		if(check(TokKind::KwStaticAssert)) {
			if(!parseStaticAssert())
				return nullptr;
			return makeStmt(StmtKind::Empty, start.offset);
		}
		if(check(TokKind::KwTypedef)) {
			if(!parseTypedef())
				return nullptr;
			return makeStmt(StmtKind::Empty, start.offset);
		}
		CType base;
		if(!parseTypeSpec(base)) {
			fail(peek(), "expected type specifier");
			return nullptr;
		}
		Stmt* s = makeStmt(StmtKind::Decl, start.offset);
		if(accept(TokKind::Semicolon))
			return s;
		if(!parseDeclarators(base, start, s, nullptr))
			return nullptr;
		for(const Declarator& d : s->decls)
			typedefs.erase(*d.name);
		return s;
	}

	// ( expr )
	Expr* Parser::parseParenCond() {
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;
		Expr* cond = parseExpression();
		if(!cond)
			return nullptr;
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;
		return cond;
	}

	// if ( expr ) stmt [ else stmt ]
	Stmt* Parser::parseIf() {
		Token kw = advance(); // if
		Expr* cond = parseParenCond();
		if(!cond)
			return nullptr;
		Stmt* thenS = parseStatement();
		if(!thenS)
			return nullptr;
		Stmt* elseS = nullptr;
		if(accept(TokKind::KwElse)) {
			elseS = parseStatement();
			if(!elseS)
				return nullptr;
		}
		Stmt* s = makeStmt(StmtKind::If, kw.offset);
		s->expr = cond;
		s->thenBody = thenS;
		s->elseBody = elseS;
		return s;
	}

	// while ( expr ) stmt
	Stmt* Parser::parseWhile() {
		Token kw = advance(); // while
		Expr* cond = parseParenCond();
		if(!cond)
			return nullptr;
		Stmt* body = parseStatement();
		if(!body)
			return nullptr;
		Stmt* s = makeStmt(StmtKind::While, kw.offset);
		s->expr = cond;
		s->thenBody = body;
		return s;
	}

	// do stmt while ( expr ) ;
	Stmt* Parser::parseDoWhile() {
		Token kw = advance(); // do
		Stmt* body = parseStatement();
		if(!body)
			return nullptr;
		if(!expect(TokKind::KwWhile, "'while'"))
			return nullptr;
		Expr* cond = parseParenCond();
		if(!cond)
			return nullptr;
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		Stmt* s = makeStmt(StmtKind::DoWhile, kw.offset);
		s->expr = cond;
		s->thenBody = body;
		return s;
	}

	// for ( for-init [expr] ; [expr] ) stmt
	// for-init: declaration | [expr] ;
	Stmt* Parser::parseFor() {
		Token kw = advance(); // for
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;

		Stmt* init = nullptr;
		if(startsType(peek()) || check(TokKind::KwTypedef)) {
			init = parseDeclaration();
			if(!init)
				return nullptr;
		} else if(!accept(TokKind::Semicolon)) {
			Token at = peek();
			Expr* e = parseExpression();
			if(!e)
				return nullptr;
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			init = makeStmt(StmtKind::Expr, at.offset);
			init->expr = e;
		}

		Expr* cond = nullptr;
		if(!check(TokKind::Semicolon)) {
			cond = parseExpression();
			if(!cond)
				return nullptr;
		}
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;

		Expr* post = nullptr;
		if(!check(TokKind::RParen)) {
			post = parseExpression();
			if(!post)
				return nullptr;
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;

		Stmt* body = parseStatement();
		if(!body)
			return nullptr;

		Stmt* s = makeStmt(StmtKind::For, kw.offset);
		s->forInit = init;
		s->expr = cond;
		s->forPost = post;
		s->thenBody = body;
		return s;
	}

	// switch ( expr ) stmt
	// a non-block body is wrapped in a compound
	Stmt* Parser::parseSwitch() {
		Token kw = advance(); // switch
		Expr* ctrl = parseParenCond();
		if(!ctrl)
			return nullptr;
		Stmt* body;
		if(check(TokKind::LBrace)) {
			body = parseStatement();
			if(!body)
				return nullptr;
		} else {
			Stmt* block = makeStmt(StmtKind::Compound, peek().offset);
			Stmt* inner = parseStatement();
			if(!inner)
				return nullptr;
			block->body.push_back(inner);
			body = block;
		}
		Stmt* s = makeStmt(StmtKind::Switch, kw.offset);
		s->expr = ctrl;
		s->thenBody = body;
		return s;
	}

	// stmt, or empty when a label ends the block
	Stmt* Parser::parseLabeledSub() {
		if(check(TokKind::RBrace) || check(TokKind::Eof))
			return makeStmt(StmtKind::Empty, peek().offset);
		return parseStatement();
	}

	// compound | asm | name : stmt | goto name ; | declaration
	// | if | while | do | for | switch | case const-expr : [stmt] | default : [stmt]
	// | break ; | continue ; | return [expr] ; | ; | expr ;
	Stmt* Parser::parseStatement() {
		DepthScope scope(*this);
		if(!enterDepth())
			return nullptr;
		const Token& tok = peek();
		if(tok.kind == TokKind::LBrace)
			return parseCompound();

		if(tok.kind == TokKind::KwAsm)
			return parseAsmStatement();

		if(tok.kind == TokKind::Identifier && peek2().kind == TokKind::Colon) {
			Token nameTok = advance();
			advance(); // ':'
			Stmt* sub = parseStatement();
			if(!sub)
				return nullptr;
			Stmt* s = makeStmt(StmtKind::Label, nameTok.offset);
			s->label = arena.make<String>(lex.text(nameTok));
			s->thenBody = sub;
			return s;
		}

		if(tok.kind == TokKind::KwGoto) {
			Token kw = advance();
			if(!check(TokKind::Identifier)) {
				fail(peek(), "expected a label name after 'goto'");
				return nullptr;
			}
			Token nameTok = advance();
			Stmt* s = makeStmt(StmtKind::Goto, kw.offset);
			s->label = arena.make<String>(lex.text(nameTok));
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if(startsType(tok) || tok.kind == TokKind::KwTypedef || tok.kind == TokKind::KwStaticAssert)
			return parseDeclaration();

		if(tok.kind == TokKind::KwIf)
			return parseIf();
		if(tok.kind == TokKind::KwWhile)
			return parseWhile();
		if(tok.kind == TokKind::KwDo)
			return parseDoWhile();
		if(tok.kind == TokKind::KwFor)
			return parseFor();
		if(tok.kind == TokKind::KwSwitch)
			return parseSwitch();

		if(tok.kind == TokKind::KwCase) {
			Token kw = advance();
			Expr* value = parseConditional();
			if(!value)
				return nullptr;
			if(!expect(TokKind::Colon, "':'"))
				return nullptr;
			Stmt* sub = parseLabeledSub();
			if(!sub)
				return nullptr;
			Stmt* s = makeStmt(StmtKind::Case, kw.offset);
			s->expr = value;
			s->thenBody = sub;
			return s;
		}

		if(tok.kind == TokKind::KwDefault) {
			Token kw = advance();
			if(!expect(TokKind::Colon, "':'"))
				return nullptr;
			Stmt* sub = parseLabeledSub();
			if(!sub)
				return nullptr;
			Stmt* s = makeStmt(StmtKind::Default, kw.offset);
			s->thenBody = sub;
			return s;
		}

		if(tok.kind == TokKind::KwBreak || tok.kind == TokKind::KwContinue) {
			Token kw = advance();
			Stmt* s =
					makeStmt(kw.kind == TokKind::KwBreak ? StmtKind::Break : StmtKind::Continue, kw.offset);
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if(tok.kind == TokKind::KwReturn) {
			Token kw = advance();
			Stmt* s = makeStmt(StmtKind::Return, kw.offset);
			if(!check(TokKind::Semicolon)) {
				s->expr = parseExpression();
				if(!s->expr)
					return nullptr;
			}
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			return s;
		}

		if(tok.kind == TokKind::Semicolon) {
			Token semi = advance();
			return makeStmt(StmtKind::Empty, semi.offset);
		}

		Stmt* s = makeStmt(StmtKind::Expr, tok.offset);
		s->expr = parseExpression();
		if(!s->expr)
			return nullptr;
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	// { [stmt]... }
	Stmt* Parser::parseCompound() {
		Token open = peek();
		if(!expect(TokKind::LBrace, "'{'"))
			return nullptr;
		Stmt* block = makeStmt(StmtKind::Compound, open.offset);
		pushScope();
		while(!failed && !check(TokKind::RBrace) && !check(TokKind::Eof)) {
			Stmt* s = parseStatement();
			if(!s)
				return nullptr;
			block->body.push_back(s);
		}
		popScope();
		if(!expect(TokKind::RBrace, "'}'"))
			return nullptr;
		return block;
	}

	void Parser::pushScope() {
		typedefs.push();
		enumConstants.push();
		enumSignedTags.push();
		structTypes.push();
		++scopeDepth;
	}

	void Parser::popScope() {
		--scopeDepth;
		typedefs.pop();
		enumConstants.pop();
		enumSignedTags.pop();
		structTypes.pop();
	}

} // namespace rat::cc
