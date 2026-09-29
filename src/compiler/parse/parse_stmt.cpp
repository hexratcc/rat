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
		if(!parseStrings(msg, "_Static_assert"))
			return false;
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
			init = parseDeclaration(nullptr);
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

	// name : stmt
	Stmt* Parser::parseLabel() {
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

	// goto name ;
	Stmt* Parser::parseGoto() {
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

	// case const-expr : [stmt] | default : [stmt]
	Stmt* Parser::parseCaseLabel() {
		Token kw = advance();
		B32 isCase = kw.kind == TokKind::KwCase;
		Expr* value = nullptr;
		if(isCase) {
			value = parseConditional();
			if(!value)
				return nullptr;
		}
		if(!expect(TokKind::Colon, "':'"))
			return nullptr;
		Stmt* sub = parseLabeledSub();
		if(!sub)
			return nullptr;
		Stmt* s = makeStmt(isCase ? StmtKind::Case : StmtKind::Default, kw.offset);
		s->expr = value;
		s->thenBody = sub;
		return s;
	}

	// break ; | continue ;
	Stmt* Parser::parseBreak() {
		Token kw = advance();
		Stmt* s =
				makeStmt(kw.kind == TokKind::KwBreak ? StmtKind::Break : StmtKind::Continue, kw.offset);
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	// return [expr] ;
	Stmt* Parser::parseReturn() {
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

	// expr ;
	Stmt* Parser::parseExprStatement() {
		Stmt* s = makeStmt(StmtKind::Expr, peek().offset);
		s->expr = parseExpression();
		if(!s->expr)
			return nullptr;
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	// compound | asm | name : stmt | goto name ; | declaration
	// | if | while | do | for | switch | case const-expr : [stmt] | default : [stmt]
	// | break ; | continue ; | return [expr] ; | ; | expr ;
	Stmt* Parser::parseStatement() {
		DepthScope scope(*this);
		if(!enterDepth())
			return nullptr;
		const Token& tok = peek();
		if(tok.kind == TokKind::Identifier && peek2().kind == TokKind::Colon)
			return parseLabel();
		if(startsType(tok) || tok.kind == TokKind::KwTypedef || tok.kind == TokKind::KwStaticAssert)
			return parseDeclaration(nullptr);
		switch(tok.kind) {
		case TokKind::LBrace:
			return parseCompound();
		case TokKind::KwAsm:
			return parseAsmStatement();
		case TokKind::KwGoto:
			return parseGoto();
		case TokKind::KwIf:
			return parseIf();
		case TokKind::KwWhile:
			return parseWhile();
		case TokKind::KwDo:
			return parseDoWhile();
		case TokKind::KwFor:
			return parseFor();
		case TokKind::KwSwitch:
			return parseSwitch();
		case TokKind::KwCase:
		case TokKind::KwDefault:
			return parseCaseLabel();
		case TokKind::KwBreak:
		case TokKind::KwContinue:
			return parseBreak();
		case TokKind::KwReturn:
			return parseReturn();
		case TokKind::Semicolon:
			return makeStmt(StmtKind::Empty, advance().offset);
		default:
			return parseExprStatement();
		}
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
