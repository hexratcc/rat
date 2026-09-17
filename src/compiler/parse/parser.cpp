#include "parse/parser.h"

#include <sstream>

namespace rat::cc {
	B32 Parser::enterDepth() {
		if(++parseDepth > kMaxParseDepth) {
			fail(peek(), "nesting too deep");
			return false;
		}
		return true;
	}

	Parser::Parser(TokenStream& lexer, Arena& arena, const TargetLayout& layout)
	: lex(lexer),
		arena(arena),
		lay(layout) {
		CType vaList;
		vaList.bits = 8;
		vaList.set(CType::Unsigned);
		if(lay.win64VaList) {
			// Win64 __builtin_va_list is a char*
			vaList.ptr = 1;
		} else {
			// SysV __builtin_va_list decays to a pointer to a 24-byte state record
			ArrayType* vl = arena.make<ArrayType>();
			vl->elem = CType{}; // unsigned char
			vl->elem.bits = 8;
			vl->elem.set(CType::Unsigned);
			vl->count = lay.ptrBytes * 4;
			vaList.array = vl;
		}
		typedefs.set("va_list", vaList);
		typedefs.set("__builtin_va_list", vaList);
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

	B32 Parser::expect(TokKind kind, const C8* what) {
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

	Expr* Parser::makeInt(const Token& tok, I64 value, U32 bits, U8 mods) {
		Expr* e = makeExpr(ExprKind::IntLit, tok.offset);
		if(mods & CType::LongLong) // 'long long' subsumes 'long'
			mods |= CType::Long;
		e->intLit = {value, bits, mods};
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

	// no two named parameters share a name
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

	// a function body may be given once per unit
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

	// the FuncDef of a function declarator, with the storage of its type-spec
	FuncDef* Parser::makeFuncDef(const DeclResult& r, const Token& start, const DeclSpecs& ds) {
		const FuncType* ft = r.type.func;
		FuncDef* fn = arena.make<FuncDef>();
		fn->name = *r.name;
		fn->retType = ft->ret;
		fn->params = ft->params;
		fn->isVarArgs = ft->isVarArgs;
		fn->unprototyped = ft->unprototyped;
		fn->isExternInline = ds.isExtern && ds.isInline;
		fn->isStatic = ds.isStatic;
		fn->isNoInline = ds.isNoInline;
		fn->align = r.align;
		fn->offset = start.offset;
		return fn;
	}

	// [ type-spec declarator [, declarator]... ; ]...
	// each declarator names a parameter of the old-style list and gives it its type
	B32 Parser::parseOldStyleDecls(FuncDef* fn) {
		while(startsType(peek())) {
			CType base;
			if(!parseTypeSpec(base)) {
				fail(peek(), "expected parameter type");
				return false;
			}
			for(;;) {
				DeclResult r;
				if(!parseDeclarator(base, r))
					return false;
				if(!r.name) {
					fail(peek(), "expected parameter name");
					return false;
				}
				adjustParamType(r.type);
				B32 matched = false;
				for(Param& p : fn->params) {
					if(p.name && *p.name == *r.name) {
						p.type = r.type;
						matched = true;
						break;
					}
				}
				if(!matched) {
					fail(peek(), "parameter named in declaration is not in the identifier list");
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

	// [old-style-decls] compound
	B32 Parser::parseFunctionDef(FuncDef* fn, B32 oldStyle) {
		if(oldStyle && !parseOldStyleDecls(fn))
			return false;
		if(!checkParamNames(fn))
			return false;
		curFuncName = fn->name;
		fn->body = parseCompound();
		curFuncName.clear();
		return fn->body != nullptr;
	}

	// [attrs] [function-def]
	// a prototype goes to the unit, or to blockProtos inside a block; defined reports a body
	B32 Parser::parseFunctionDeclarator(
			const DeclResult& r, const Token& start, const DeclSpecs& ds, TransUnit* unit, B32& defined) {
		FuncDef* fn = makeFuncDef(r, start, ds);
		B32 noInline = false;
		if(!parseDeclAttributes(fn->aliasOf, noInline, fn->align))
			return false;
		fn->isNoInline |= noInline;
		B32 oldStyle = r.type.func->oldStyle;
		defined = check(TokKind::LBrace) || (oldStyle && startsType(peek()));
		if(oldStyle && !defined) {
			fail(peek(), "parameter names (without types) in function declaration");
			return false;
		}
		if(defined) {
			if(!parseFunctionDef(fn, oldStyle))
				return false;
			if(unit && !registerFuncDef(fn))
				return false;
		}
		List<FuncDef*>& into = unit ? unit->functions : blockProtos;
		into.push_back(fn);
		return true;
	}

	// [attrs] [= initializer]
	// an extern object may only take an initializer at file scope
	B32 Parser::parseObjectDeclarator(
			const DeclResult& r, const Token& start, const DeclSpecs& ds, Stmt* s, B32 fileScope) {
		Declarator d;
		d.isExtern = ds.isExtern;
		d.isStatic = ds.isStatic;
		bindDeclarator(d, r);
		B32 noInline = false;
		if(!parseDeclAttributes(d.aliasOf, noInline, d.align))
			return false;
		if(accept(TokKind::Assign)) {
			if(!fileScope && d.isExtern) {
				fail(start, "'extern' variable cannot have an initializer");
				return false;
			}
			d.init = parseInitializer();
			if(!d.init)
				return false;
		}
		if(!checkObjectComplete(d))
			return false;
		s->decls.push_back(d);
		return true;
	}

	// init-declarator [, init-declarator]... ;
	// init-declarator: declarator function-declarator | declarator object-declarator
	// a function body ends the list without the ;
	// objects go to s, functions to the unit or, in a block, to blockProtos
	B32 Parser::parseDeclarators(CType base, const Token& start, Stmt* s, TransUnit* unit) {
		DeclSpecs ds = specs;
		U32 align = specAlign;
		for(;;) {
			DeclResult r;
			r.allowOldStyle = true;
			r.align = align;
			if(!parseDeclarator(base, r))
				return false;
			if(!r.name) {
				fail(peek(), "expected declarator name");
				return false;
			}
			if(r.type.func && r.type.ptr == 0) {
				B32 defined = false;
				if(!parseFunctionDeclarator(r, start, ds, unit, defined))
					return false;
				if(defined)
					return true;
			} else if(!parseObjectDeclarator(r, start, ds, s, unit != nullptr)) {
				return false;
			}
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::Semicolon, "';'");
	}

	// static-assert | typedef | type-spec ; | type-spec declarators
	// the Decl stmt lists the objects; functions go to the unit or, in a block, to blockProtos
	Stmt* Parser::parseDeclaration(TransUnit* unit) {
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
		if(!parseDeclarators(base, start, s, unit))
			return nullptr;
		for(const Declarator& d : s->decls)
			typedefs.erase(*d.name);
		return s;
	}

	// [ ; | asm-stmt | declaration ]... eof
	// a file-scope asm-stmt must have an empty template
	TransUnit* Parser::parseUnit() {
		TransUnit* unit = arena.make<TransUnit>();
		while(!failed && !check(TokKind::Eof)) {
			Token start = peek();
			if(accept(TokKind::Semicolon))
				continue;
			if(check(TokKind::KwAsm)) {
				Stmt* s = parseAsmStatement();
				if(!s)
					return nullptr;
				if(!s->asmBlock->text->empty()) {
					fail(start, "file-scope assembly is not supported");
					return nullptr;
				}
				continue;
			}
			Stmt* s = parseDeclaration(unit);
			if(!s)
				return nullptr;
			if(!s->decls.empty())
				unit->globals.push_back(s);
		}
		if(failed)
			return nullptr;
		for(FuncDef* proto : blockProtos)
			unit->functions.push_back(proto);
		return unit;
	}
} // namespace rat::cc
