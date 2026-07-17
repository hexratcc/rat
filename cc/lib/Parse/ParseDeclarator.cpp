#include "Parse/Parser.h"

#include "Parse/ParserDetail.h"

namespace rat::cc {
	void Parser::skipArrayQualifiers() {
		while(peek().kind == TokKind::KwStatic || detail::isTypeQualifier(peek().kind))
			advance();
	}

	CType Parser::wrapArrayDims(CType base, const List<Dim>& dims) {
		for(U32 i = (U32)dims.size(); i-- > 0;) {
			ArrayType* at = arena.make<ArrayType>();
			at->elem = base;
			at->count = dims[i].count;
			at->countExpr = dims[i].expr;
			CType arr;
			arr.array = at;
			base = arr;
		}
		return base;
	}

	B32 Parser::parseArraySuffix(Declarator& d) {
		if(!check(TokKind::LBracket))
			return true;
		advance(); // [
		d.isArray = true;
		skipArrayQualifiers();
		if(peek().kind == TokKind::Star && peek2().kind == TokKind::RBracket) {
			advance(); // [*]
		} else if(peek().kind != TokKind::RBracket) {
			d.arrayLen = parseConditional(); // outermost bound
			if(!d.arrayLen)
				return false;
		}
		if(!expect(TokKind::RBracket, "']'"))
			return false;
		List<Dim> inner;
		while(accept(TokKind::LBracket)) {
			Expr* sz = parseConditional();
			if(!sz)
				return false;
			if(!expect(TokKind::RBracket, "']'"))
				return false;
			I64 n = 0;
			if(tryEvalIntConst(sz, n)) {
				if(n <= 0) {
					fail(peek(), "array dimension must be a positive integer constant");
					return false;
				}
				inner.push_back(Dim{(U32)n, nullptr});
			} else {
				inner.push_back(Dim{0, sz});
			}
		}
		d.type = wrapArrayDims(d.type, inner);
		return true;
	}

	B32 Parser::parseParamArray(CType& t) {
		Declarator d;
		d.type = t;
		if(!parseArraySuffix(d))
			return false;
		CType decayed = d.type;
		++decayed.ptr;
		t = decayed;
		return true;
	}

	B32 Parser::looksLikeFuncPtr() {
		return peek().kind == TokKind::LParen && peek2().kind == TokKind::Star;
	}

	void Parser::adjustParamType(CType& t, const Expr** vlaBound) {
		if(t.func != nullptr && t.ptr == 0) {
			t.ptr = 1;
		} else if(t.array != nullptr && t.ptr == 0) {
			if(vlaBound && t.array->countExpr)
				*vlaBound = t.array->countExpr;
			t = decay(t);
		}
	}

	B32 Parser::parseParamTypeList(FuncType* ft) {
		if(peek().kind == TokKind::RParen) {
			advance();
			ft->unprototyped = true;
			return true;
		}
		if(peek().kind == TokKind::KwVoid && peek2().kind == TokKind::RParen) {
			advance(); // void
			advance(); // )
			return true;
		}
		for(;;) {
			if(peek().kind == TokKind::Ellipsis) {
				advance();
				ft->isVarArgs = true;
				break;
			}
			CType pt;
			if(!parseTypeSpec(pt)) {
				fail(peek(), "expected parameter type");
				return false;
			}
			parsePointers(pt);
			const String* pname = nullptr;
			Token nameTok;
			B32 haveName = false;
			CType fpt;
			if(!parseDeclaratorType(pt, nameTok, haveName, fpt))
				return false;
			if(haveName)
				pname = arena.make<String>(lex.text(nameTok));
			pt = fpt;
			adjustParamType(pt);
			ft->params.push_back(pt);
			ft->paramNames.push_back(pname);
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::RParen, "')'");
	}

	B32 Parser::parseFuncPtrDeclarator(CType ret, Token& nameOut, CType& outType) {
		B32 haveName = false;
		return parseDeclaratorType(ret, nameOut, haveName, outType);
	}

	void Parser::bindDeclaratorType(Declarator& d, CType t, U32 offset) {
		if(isArrayType(t)) {
			d.isArray = true;
			U32 count = t.array->count;
			d.type = t.array->elem;
			if(count > 0) {
				Expr* len = makeExpr(ExprKind::IntLit, offset);
				len->intLit = {(I64)count, false, 32, false, false};
				d.arrayLen = len;
			}
		} else {
			d.type = t;
		}
	}

	B32 Parser::looksLikeGroupingParen() {
		if(peek().kind != TokKind::LParen)
			return false;
		const Token& n = peek2();
		if(n.kind == TokKind::Star || n.kind == TokKind::LParen || n.kind == TokKind::LBracket)
			return true;
		if(n.kind == TokKind::Identifier)
			return !startsType(n);
		return false;
	}

	Parser::TypeBuilder Parser::parseDeclaratorSuffixes() {
		List<TypeBuilder> sfx;
		for(;;) {
			if(peek().kind == TokKind::LBracket) {
				advance(); // [
				skipArrayQualifiers();
				U32 count = 0;
				Expr* countExpr = nullptr;
				if(peek().kind == TokKind::Star && peek2().kind == TokKind::RBracket) {
					advance(); // [*]
				} else if(peek().kind != TokKind::RBracket) {
					Expr* e = parseConditional();
					if(!e)
						return {};
					I64 n = 0;
					if(tryEvalIntConst(e, n) && n > 0)
						count = (U32)n;
					else
						countExpr = e; // VLA
				}
				if(!expect(TokKind::RBracket, "']'"))
					return {};
				sfx.push_back([this, count, countExpr](CType b) {
					ArrayType* at = arena.make<ArrayType>();
					at->elem = b;
					at->count = count;
					at->countExpr = countExpr;
					CType a;
					a.array = at;
					return a;
				});
			} else if(peek().kind == TokKind::LParen) {
				advance(); // '('
				FuncType* ft = arena.make<FuncType>();
				if(!parseParamTypeList(ft))
					return {};
				sfx.push_back([ft](CType b) {
					ft->ret = b;
					CType t;
					t.func = ft;
					return t;
				});
			} else {
				break;
			}
		}
		return [sfx](CType base) {
			CType b = base;
			for(U32 i = (U32)sfx.size(); i-- > 0;)
				b = sfx[i](b);
			return b;
		};
	}

	Parser::TypeBuilder Parser::parseDirectDeclarator(Token& nameOut, B32& haveName) {
		TypeBuilder core;
		if(looksLikeGroupingParen()) {
			advance(); // (
			core = parseDeclaratorBuilder(nameOut, haveName);
			if(!expect(TokKind::RParen, "')'"))
				return {};
		} else if(peek().kind == TokKind::Identifier) {
			nameOut = advance();
			haveName = true;
			core = [](CType b) { return b; };
		} else {
			// abstract decl
			core = [](CType b) { return b; };
		}
		TypeBuilder suf = parseDeclaratorSuffixes();
		if(failed)
			return {};
		return [core, suf](CType base) { return core(suf(base)); };
	}

	Parser::TypeBuilder Parser::parseDeclaratorBuilder(Token& nameOut, B32& haveName) {
		U32 stars = 0;
		for(;;) {
			while(detail::isTypeQualifier(peek().kind))
				advance();
			if(!accept(TokKind::Star))
				break;
			++stars;
		}
		TypeBuilder inner = parseDirectDeclarator(nameOut, haveName);
		if(failed)
			return {};
		return [stars, inner](CType base) {
			CType b = base;
			for(U32 i = 0; i < stars; ++i)
				b = pointerTo(b);
			return inner(b);
		};
	}

	B32 Parser::parseDeclaratorType(CType base, Token& nameOut, B32& haveName, CType& out) {
		haveName = false;
		TypeBuilder b = parseDeclaratorBuilder(nameOut, haveName);
		if(failed)
			return false;
		out = b(base);
		return true;
	}

} // namespace rat::cc
