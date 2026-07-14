#include "Parse/Parser.h"

namespace rat::cc {
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
		if(peek().kind != TokKind::StringLiteral) {
			fail(peek(), "expected a string literal in _Static_assert");
			return false;
		}
		if(!parseStringLiteral(advance(), msg))
			return false;
		while(peek().kind == TokKind::StringLiteral) {
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

	Stmt* Parser::parseDeclaration() {
		Token start = peek();
		if(peek().kind == TokKind::KwStaticAssert || peek().kind == TokKind::KwTypedef) {
			B32 ok = peek().kind == TokKind::KwStaticAssert ? parseStaticAssert() : parseTypedef();
			if(!ok)
				return nullptr;
			return makeStmt(StmtKind::Empty, start.offset);
		}
		CType base;
		if(!parseTypeSpec(base)) {
			fail(peek(), "expected type specifier");
			return nullptr;
		}
		B32 isStatic = sawStatic;
		B32 isExtern = sawExtern;
		Stmt* s = makeStmt(StmtKind::Decl, start.offset);
		if(peek().kind == TokKind::Semicolon) {
			advance();
			return s;
		}
		for(;;) {
			CType t = base;
			parsePointers(t);
			Declarator d;
			d.isStatic = isStatic;
			d.isExtern = isExtern;
			if(looksLikeGroupingParen()) {
				Token nameTok;
				B32 haveName = false;
				CType gt;
				if(!parseDeclaratorType(t, nameTok, haveName, gt))
					return nullptr;
				if(!haveName) {
					fail(peek(), "expected declarator name");
					return nullptr;
				}
				d.name = arena.make<String>(lex.text(nameTok));
				bindDeclaratorType(d, gt, nameTok.offset);
				d.offset = nameTok.offset;
			} else {
				if(peek().kind != TokKind::Identifier) {
					fail(peek(), "expected declarator name");
					return nullptr;
				}
				Token nameTok = advance();
				if(peek().kind == TokKind::LParen) {
					B32 more = false;
					FuncDef* proto = parseFunctionRest(t, nameTok, start, &more);
					if(!proto)
						return nullptr;
					blockProtos.push_back(proto);
					if(more)
						continue;
					return s;
				}
				d.name = arena.make<String>(lex.text(nameTok));
				d.type = t;
				d.offset = nameTok.offset;
				if(!parseArraySuffix(d))
					return nullptr;
				if(!d.isArray)
					bindDeclaratorType(d, t, nameTok.offset);
			}
			if(accept(TokKind::Assign)) {
				if(isExtern) {
					fail(start, "'extern' variable cannot have an initializer");
					return nullptr;
				}
				d.init = parseInitializer();
				if(!d.init)
					return nullptr;
			}
			if(!checkObjectComplete(d))
				return nullptr;
			s->decls.push_back(d);
			if(!accept(TokKind::Comma))
				break;
		}
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

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

	Stmt* Parser::parseFor() {
		Token kw = advance(); // for
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;

		// init clause
		Stmt* init = nullptr;
		if(startsType(peek()) || peek().kind == TokKind::KwTypedef) {
			init = parseDeclaration();
			if(!init)
				return nullptr;
		} else if(peek().kind != TokKind::Semicolon) {
			Token at = peek();
			Expr* e = parseExpression();
			if(!e)
				return nullptr;
			if(!expect(TokKind::Semicolon, "';'"))
				return nullptr;
			init = makeStmt(StmtKind::Expr, at.offset);
			init->expr = e;
		} else {
			advance(); // empty init
		}

		// condition
		Expr* cond = nullptr;
		if(peek().kind != TokKind::Semicolon) {
			cond = parseExpression();
			if(!cond)
				return nullptr;
		}
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;

		// post expression
		Expr* post = nullptr;
		if(peek().kind != TokKind::RParen) {
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

	Stmt* Parser::parseSwitch() {
		Token kw = advance(); // switch
		Expr* ctrl = parseParenCond();
		if(!ctrl)
			return nullptr;
		Stmt* body;
		if(peek().kind == TokKind::LBrace) {
			body = parseStatement();
			if(!body)
				return nullptr;
		} else {
			Stmt* block = makeStmt(StmtKind::Compound, peek().offset);
			while(peek().kind == TokKind::KwCase || peek().kind == TokKind::KwDefault) {
				Stmt* marker = parseStatement();
				if(!marker)
					return nullptr;
				block->body.push_back(marker);
			}
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

	Stmt* Parser::parseStatement() {
		const Token& tok = peek();
		if(tok.kind == TokKind::LBrace)
			return parseCompound();

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
			Stmt* s = makeStmt(StmtKind::Case, kw.offset);
			s->expr = value;
			return s;
		}

		if(tok.kind == TokKind::KwDefault) {
			Token kw = advance();
			if(!expect(TokKind::Colon, "':'"))
				return nullptr;
			return makeStmt(StmtKind::Default, kw.offset);
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
			if(peek().kind != TokKind::Semicolon) {
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

	Stmt* Parser::parseCompound() {
		Token open = peek();
		if(!expect(TokKind::LBrace, "'{'"))
			return nullptr;
		Stmt* block = makeStmt(StmtKind::Compound, open.offset);
		Map<String, CType> savedTypedefs = typedefs;
		Map<String, I64> savedEnumConstants = enumConstants;
		Map<String, StructType*> savedStructTypes = structTypes;
		while(!failed && peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
			Stmt* s = parseStatement();
			if(!s)
				return nullptr;
			block->body.push_back(s);
		}
		typedefs = std::move(savedTypedefs);
		enumConstants = std::move(savedEnumConstants);
		structTypes = std::move(savedStructTypes);
		if(!expect(TokKind::RBrace, "'}'"))
			return nullptr;
		return block;
	}

} // namespace rat::cc
