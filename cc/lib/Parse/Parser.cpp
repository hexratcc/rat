#include "Parse/Parser.h"

namespace rat::cc {
	Parser::Parser(Lexer& lexer, Arena& arena, const TargetInfo& target)
	: lex(lexer),
		arena(arena),
		target(target) {
		ArrayType* vl = arena.make<ArrayType>();
		vl->elem = CType{8, true, false, 0}; // unsigned char
		vl->count = target.getPointerSizeInBytes() * 4;
		CType vaList{8, true, false, 0};
		vaList.array = vl;
		typedefs["va_list"] = vaList;
		typedefs["__builtin_va_list"] = vaList;
	}

	void Parser::fail(const Token& at, const String& msg) {
		if(failed)
			return;
		std::ostringstream os;
		os << lex.file() << ":" << at.line << ":" << at.col << ": " << msg;
		errMsg = os.str();
		failed = true;
	}

	B32 Parser::accept(TokKind kind) {
		if(peek().kind == kind) {
			advance();
			return true;
		}
		return false;
	}

	B32 Parser::expect(TokKind kind, const char* what) {
		if(peek().kind == kind) {
			advance();
			return true;
		}
		fail(peek(), String("expected ") + what + ", found '" + tokKindName(peek().kind) + "'");
		return false;
	}

	Expr* Parser::makeExpr(ExprKind kind, U32 offset) {
		Expr* e = arena.make<Expr>();
		e->kind = kind;
		e->offset = offset;
		return e;
	}

	Stmt* Parser::makeStmt(StmtKind kind, U32 offset) {
		Stmt* s = arena.make<Stmt>();
		s->kind = kind;
		s->offset = offset;
		return s;
	}

	Expr* Parser::makeInt(const Token& tok, I64 value, B32 isUnsigned, B32 isLong) {
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

	B32 Parser::checkParamNames(const FuncDef* fn) {
		for(U32 i = 0; i < fn->params.size(); i++) {
			if(!fn->params[i].name)
				continue;
			for(U32 j = 0; j < i; j++) {
				if(fn->params[j].name && *fn->params[j].name == *fn->params[i].name) {
					fail(peek(), "redefinition of parameter");
					return false;
				}
			}
		}
		return true;
	}

	FuncDef* Parser::parseFunctionRest(CType ret,
																		 const Token& nameTok,
																		 const Token& start,
																		 B32* moreDeclarators) {
		if(!expect(TokKind::LParen, "'('"))
			return nullptr;

		FuncDef* fn = arena.make<FuncDef>();
		fn->name = lex.text(nameTok);
		fn->retType = ret;
		fn->offset = start.offset;

		if(peek().kind == TokKind::RParen) {
			fn->unprototyped = true;
		} else if(peek().kind == TokKind::KwVoid && peek2().kind == TokKind::RParen) {
			advance(); // (void)
		} else if(peek().kind == TokKind::Identifier && !startsType(peek())) {
			if(!parseOldStyleParams(fn))
				return nullptr;
			if(!checkParamNames(fn))
				return nullptr;
			curFuncName = fn->name;
			fn->body = parseCompound();
			curFuncName.clear();
			return fn->body ? fn : nullptr;
		} else if(peek().kind != TokKind::RParen) {
			for(;;) {
				if(peek().kind == TokKind::Ellipsis) {
					if(fn->params.empty()) {
						fail(peek(), "'...' must be preceded by a named parameter");
						return nullptr;
					}
					advance();
					fn->isVarArgs = true;
					break;
				}
				Token pstart = peek();
				CType pt;
				if(!parseTypeSpec(pt)) {
					fail(peek(), "expected parameter type");
					return nullptr;
				}
				parsePointers(pt);
				Param p;
				p.offset = pstart.offset;
				Token pnameTok;
				B32 haveName = false;
				CType fpt;
				if(!parseDeclaratorType(pt, pnameTok, haveName, fpt))
					return nullptr;
				pt = fpt;
				if(isVoidType(pt)) {
					fail(pstart, "'void' must be the only unnamed parameter");
					return nullptr;
				}
				adjustParamType(pt, &p.vlaBound);
				p.type = pt;
				if(haveName)
					p.name = arena.make<String>(lex.text(pnameTok));
				fn->params.push_back(p);
				if(!accept(TokKind::Comma))
					break;
			}
		}
		if(!expect(TokKind::RParen, "')'"))
			return nullptr;

		if(accept(TokKind::Semicolon))
			return fn;

		if(moreDeclarators && peek().kind == TokKind::Comma) {
			advance();
			*moreDeclarators = true;
			return fn;
		}

		if(!checkParamNames(fn))
			return nullptr;
		curFuncName = fn->name;
		fn->body = parseCompound();
		curFuncName.clear();
		if(!fn->body)
			return nullptr;
		return fn;
	}

	B32 Parser::parseOldStyleParams(FuncDef* fn) {
		for(;;) {
			Token nameTok = peek();
			if(nameTok.kind != TokKind::Identifier) {
				fail(nameTok, "expected parameter name");
				return false;
			}
			advance();
			Param p;
			p.name = arena.make<String>(lex.text(nameTok));
			p.type = ctInt();
			p.offset = nameTok.offset;
			fn->params.push_back(p);
			if(!accept(TokKind::Comma))
				break;
		}
		if(!expect(TokKind::RParen, "')'"))
			return false;
		while(startsType(peek())) {
			CType base;
			if(!parseTypeSpec(base)) {
				fail(peek(), "expected parameter type");
				return false;
			}
			for(;;) {
				CType t = base;
				Token pnameTok;
				B32 haveName = false;
				CType decl;
				if(!parseDeclaratorType(t, pnameTok, haveName, decl))
					return false;
				if(!haveName) {
					fail(peek(), "expected parameter name");
					return false;
				}
				adjustParamType(decl);
				String name = lex.text(pnameTok);
				B32 matched = false;
				for(U32 i = 0; i < fn->params.size(); i++) {
					if(fn->params[i].name && *fn->params[i].name == name) {
						fn->params[i].type = decl;
						matched = true;
						break;
					}
				}
				if(!matched) {
					fail(pnameTok,
							 "parameter named in declaration is not in the "
							 "identifier list");
					return false;
				}
				if(!accept(TokKind::Comma))
					break;
			}
			if(!expect(TokKind::Semicolon, "';'"))
				return false;
		}
		return true;
	}

	Stmt* Parser::parseGlobalRest(CType base, Declarator first, const Token& start) {
		Stmt* s = makeStmt(StmtKind::Decl, start.offset);
		Declarator d = first;
		for(;;) {
			CType raw = d.type;
			if(!parseArraySuffix(d))
				return nullptr;
			if(!d.isArray)
				bindDeclaratorType(d, raw, d.offset);
			if(accept(TokKind::Assign)) {
				d.init = parseInitializer();
				if(!d.init)
					return nullptr;
			}
			if(!checkObjectComplete(d))
				return nullptr;
			s->decls.push_back(d);
			if(!accept(TokKind::Comma))
				break;
			CType t = base;
			parsePointers(t);
			Declarator prev = d;
			d = Declarator{};
			d.isExtern = prev.isExtern;
			d.isStatic = prev.isStatic;
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
				d.name = arena.make<String>(lex.text(nameTok));
				d.type = t;
				d.offset = nameTok.offset;
			}
		}
		if(!expect(TokKind::Semicolon, "';'"))
			return nullptr;
		return s;
	}

	B32 Parser::parseSharedDeclarators(CType base, TransUnit* unit, const Token& start) {
		for(;;) {
			CType t = base;
			parsePointers(t);
			if(peek().kind != TokKind::Identifier) {
				fail(peek(), "expected declarator name");
				return false;
			}
			Token nameTok = advance();
			if(peek().kind == TokKind::LParen) {
				B32 more = false;
				FuncDef* fn = parseFunctionRest(t, nameTok, start, &more);
				if(!fn)
					return false;
				unit->functions.push_back(fn);
				if(more)
					continue;
				return true;
			}
			Declarator d;
			d.name = arena.make<String>(lex.text(nameTok));
			d.type = t;
			d.offset = nameTok.offset;
			Stmt* g = parseGlobalRest(base, d, start);
			if(!g)
				return false;
			unit->globals.push_back(g);
			return true;
		}
	}

	B32 Parser::registerFuncDef(FuncDef* fn) {
		if(!fn->body)
			return true;
		auto it = funcDefs.find(fn->name);
		if(it != funcDefs.end()) {
			fail(peek(), "redefinition of function");
			return false;
		}
		funcDefs.emplace(fn->name, fn);
		return true;
	}

	TransUnit* Parser::parseUnit() {
		TransUnit* unit = arena.make<TransUnit>();
		while(!failed && peek().kind != TokKind::Eof) {
			Token start = peek();
			if(peek().kind == TokKind::KwStaticAssert) {
				if(!parseStaticAssert())
					return nullptr;
				continue;
			}
			if(peek().kind == TokKind::KwTypedef) {
				if(!parseTypedef())
					return nullptr;
				continue;
			}
			CType base;
			if(!parseTypeSpec(base)) {
				fail(peek(), "expected type specifier");
				return nullptr;
			}
			B32 gExtern = sawExtern;
			B32 gExternInline = sawExtern && sawInline;
			B32 gStatic = sawStatic;
			CType first = base;
			parsePointers(first);
			if(first.ptr == 0 && peek().kind == TokKind::Semicolon) {
				advance();
				continue;
			}

			if(looksLikeFuncPtr()) {
				Token nameTok;
				CType fpt;
				if(!parseFuncPtrDeclarator(first, nameTok, fpt))
					return nullptr;

				if(fpt.func && fpt.ptr == 0) {
					FuncDef* fn = arena.make<FuncDef>();
					fn->name = lex.text(nameTok);
					fn->retType = fpt.func->ret;
					fn->isVarArgs = fpt.func->isVarArgs;
					fn->isStatic = gStatic;
					fn->offset = start.offset;
					for(U32 i = 0; i < fpt.func->params.size(); ++i) {
						Param p;
						p.type = fpt.func->params[i];
						if(i < fpt.func->paramNames.size())
							p.name = fpt.func->paramNames[i];
						fn->params.push_back(p);
					}
					if(accept(TokKind::Semicolon)) {
						unit->functions.push_back(fn); // prototype
						continue;
					}
					curFuncName = fn->name;
					fn->body = parseCompound();
					curFuncName.clear();
					if(!fn->body)
						return nullptr;
					if(!registerFuncDef(fn))
						return nullptr;
					unit->functions.push_back(fn);
					continue;
				}
				Declarator d;
				d.isExtern = gExtern;
				d.name = arena.make<String>(lex.text(nameTok));
				bindDeclaratorType(d, fpt, nameTok.offset);
				d.offset = nameTok.offset;
				Stmt* g = parseGlobalRest(base, d, start);
				if(!g)
					return nullptr;
				unit->globals.push_back(g);
				continue;
			}
			if(peek().kind != TokKind::Identifier) {
				fail(peek(), "expected name");
				return nullptr;
			}
			Token nameTok = advance();
			if(peek().kind == TokKind::LParen) {
				B32 more = false;
				FuncDef* fn = parseFunctionRest(first, nameTok, start, &more);
				if(!fn)
					return nullptr;
				fn->isExternInline = gExternInline;
				fn->isStatic = gStatic;
				if(!registerFuncDef(fn))
					return nullptr;
				unit->functions.push_back(fn);
				if(more) {
					if(!parseSharedDeclarators(base, unit, start))
						return nullptr;
				}
			} else {
				Declarator d;
				d.isExtern = gExtern;
				d.name = arena.make<String>(lex.text(nameTok));
				d.type = first;
				d.offset = nameTok.offset;
				Stmt* g = parseGlobalRest(base, d, start);
				if(!g)
					return nullptr;
				unit->globals.push_back(g);
			}
		}
		if(failed)
			return nullptr;
		for(FuncDef* proto : blockProtos)
			unit->functions.push_back(proto);
		return unit;
	}
} // namespace rat::cc
