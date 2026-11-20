#include "parse/parser.h"

namespace rat::cc {
	// _Static_assert ( const-expr , string-literal... ) ;
	B32 Parser::parseStaticAssert() {
		Token kw = advance(); // _Static_assert
		if(!expect(TokKind::LParen))
			return false;
		Expr* cond = parseConditional();
		String msg;
		if(!cond || !expect(TokKind::Comma) || !parseStrings(msg, "_Static_assert"))
			return false;
		if(!expect(TokKind::RParen) || !expect(TokKind::Semicolon))
			return false;
		I64 v = 0;
		if(!evalIntConst(cond, v))
			return false;
		if(v == 0)
			return fail(kw, "static assertion failed: " + msg);
		return true;
	}

	// a non-extern object must not have an incomplete struct type, through arrays
	B32 Parser::checkObjectComplete(const Declarator& d) {
		if(d.isExtern)
			return true;
		CType ult = d.type;
		while(ult.array != nullptr && ult.ptr == 0)
			ult = ult.array->elem;
		if(isStruct(ult) && !ult.strukt->complete)
			return fail(peek(), "variable has incomplete type");
		return true;
	}

	// ( expr )
	Expr* Parser::parseParenCond() {
		if(!expect(TokKind::LParen))
			return nullptr;
		Expr* cond = parseExpression();
		if(!cond || !expect(TokKind::RParen))
			return nullptr;
		return cond;
	}

	// if ( expr ) stmt [ else stmt ] | while ( expr ) stmt
	Stmt* Parser::parseCondStmt(StmtKind kind) {
		Token kw = advance(); // if / while
		Expr* cond = parseParenCond();
		if(!cond)
			return nullptr;
		Stmt* body = parseStatement();
		if(!body)
			return nullptr;
		Stmt* s = makeStmt(kind, kw.offset, cond, body);
		if(kind == StmtKind::If && accept(TokKind::KwElse)) {
			s->elseBody = parseStatement();
			if(!s->elseBody)
				return nullptr;
		}
		return s;
	}

	// do stmt while ( expr ) ;
	Stmt* Parser::parseDoWhile() {
		Token kw = advance(); // do
		Stmt* body = parseStatement();
		if(!body || !expect(TokKind::KwWhile))
			return nullptr;
		Expr* cond = parseParenCond();
		if(!cond || !expect(TokKind::Semicolon))
			return nullptr;
		return makeStmt(StmtKind::DoWhile, kw.offset, cond, body);
	}

	// for ( for-init [expr] ; [expr] ) stmt
	// for-init: declaration | [expr] ;
	// a for-init declaration is scoped to the loop
	Stmt* Parser::parseFor() {
		Token kw = advance(); // for
		if(!expect(TokKind::LParen))
			return nullptr;
		pushScope();

		Stmt* init = nullptr;
		if(startsType(peek()) || check(TokKind::KwTypedef)) {
			init = parseDeclaration(nullptr);
			if(!init)
				return nullptr;
		} else if(!accept(TokKind::Semicolon)) {
			Token at = peek();
			Expr* e = parseExpression();
			if(!e || !expect(TokKind::Semicolon))
				return nullptr;
			init = makeStmt(StmtKind::Expr, at.offset, e);
		}

		Expr* cond = nullptr;
		if(!parseOptionalExpr(TokKind::Semicolon, cond))
			return nullptr;

		Expr* post = nullptr;
		if(!parseOptionalExpr(TokKind::RParen, post))
			return nullptr;

		Stmt* body = parseStatement();
		if(!body)
			return nullptr;
		popScope();
		Stmt* s = makeStmt(StmtKind::For, kw.offset, cond, body);
		s->forInit = init;
		s->forPost = post;
		return s;
	}

	// switch ( expr ) stmt
	// a non-block body is wrapped in a compound
	Stmt* Parser::parseSwitch() {
		Token kw = advance(); // switch
		Expr* ctrl = parseParenCond();
		if(!ctrl)
			return nullptr;
		Stmt* block = nullptr;
		if(!check(TokKind::LBrace))
			block = makeStmt(StmtKind::Compound, peek().offset);
		Stmt* body = parseStatement();
		if(!body)
			return nullptr;
		if(block) {
			block->body.push_back(body);
			body = block;
		}
		return makeStmt(StmtKind::Switch, kw.offset, ctrl, body);
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
		if(!check(TokKind::Identifier))
			return fail(peek(), "expected a label name after 'goto'");
		Token nameTok = advance();
		Stmt* s = makeStmt(StmtKind::Goto, kw.offset);
		s->label = arena.make<String>(lex.text(nameTok));
		if(!expect(TokKind::Semicolon))
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
		if(!expect(TokKind::Colon))
			return nullptr;
		Stmt* sub = parseLabeledSub();
		if(!sub)
			return nullptr;
		return makeStmt(isCase ? StmtKind::Case : StmtKind::Default, kw.offset, value, sub);
	}

	// break ; | continue ;
	Stmt* Parser::parseBreak() {
		Token kw = advance();
		StmtKind kind = kw.kind == TokKind::KwBreak ? StmtKind::Break : StmtKind::Continue;
		Stmt* s = makeStmt(kind, kw.offset);
		if(!expect(TokKind::Semicolon))
			return nullptr;
		return s;
	}

	// return [expr] ;
	Stmt* Parser::parseReturn() {
		Token kw = advance();
		Stmt* s = makeStmt(StmtKind::Return, kw.offset);
		if(!parseOptionalExpr(TokKind::Semicolon, s->expr))
			return nullptr;
		return s;
	}

	// [expr] end
	B32 Parser::parseOptionalExpr(TokKind end, Expr*& out) {
		if(!check(end)) {
			out = parseExpression();
			if(!out)
				return false;
		}
		return expect(end);
	}

	// expr ;
	Stmt* Parser::parseExprStatement() {
		Stmt* s = makeStmt(StmtKind::Expr, peek().offset);
		s->expr = parseExpression();
		if(!s->expr || !expect(TokKind::Semicolon))
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
			return parseCondStmt(StmtKind::If);
		case TokKind::KwWhile:
			return parseCondStmt(StmtKind::While);
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
		if(!expect(TokKind::LBrace))
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
		if(!expect(TokKind::RBrace))
			return nullptr;
		return block;
	}

	void Parser::pushScope() {
		idents.push();
		enumTags.push();
		structTypes.push();
		++scopeDepth;
	}

	void Parser::popScope() {
		--scopeDepth;
		idents.pop();
		enumTags.pop();
		structTypes.pop();
	}
} // namespace rat::cc
