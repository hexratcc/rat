#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	namespace detail {
		B32 isTypeQualifier(TokKind kind) { return tokSyntax(kind).spec & TokSyntax::Qualifier; }

		B32 isQualOrStorage(TokKind kind) {
			return tokSyntax(kind).spec &
						 (TokSyntax::Qualifier | TokSyntax::Storage | TokSyntax::FuncSpec);
		}
	} // namespace detail

	// a type keyword, qualifier, storage class or typedef name
	B32 Parser::startsType(const Token& tok) {
		if(detail::tokSyntax(tok.kind).spec != 0)
			return true;
		return tok.kind == TokKind::Identifier && typedefType(lex.text(tok)) != nullptr;
	}

	// typedef type-spec [ declarator [, declarator]... ] ;
	// s lists the typedefs whose array bounds the emitter takes here, see bindVlaDims
	B32 Parser::parseTypedef(Stmt* s) {
		advance(); // typedef
		CType base;
		if(!parseTypeSpec(base))
			return fail(peek(), "expected type in typedef declaration");
		if(accept(TokKind::Semicolon))
			return true;
		for(;;) {
			DeclResult r;
			if(!parseDeclarator(base, r))
				return false;
			if(!r.name)
				return fail(peek(), "expected typedef name");
			CType named = r.type;
			named.named = arena.make<NamedType>(NamedType{*r.name, r.type, {}, r.offset});
			idents.set(*r.name, {Ident::Kind::Typedef, named});
			if(hasUnfoldedDim(r.type)) {
				Declarator d;
				bindDeclarator(d, r);
				d.isTypedef = true;
				s->decls.push_back(d);
				s->kind = StmtKind::Decl;
			}
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::Semicolon);
	}

	// _Alignas ( type-name | const-expr )
	// raises align, never lowers it
	B32 Parser::parseAlignasSpec(U32& align) {
		Token kw = advance(); // _Alignas
		if(!expect(TokKind::LParen))
			return false;
		U32 n = 0;
		if(startsType(peek())) {
			CType ty;
			if(!parseTypeName(ty))
				return false;
			n = typeAlignBytes(ty);
		} else {
			Expr* e = parseConditional();
			I64 v = 0;
			if(!e || !evalIntConst(e, v))
				return false;
			if(v <= 0 || (v & (v - 1)) != 0)
				return fail(kw, "_Alignas requires a positive power-of-two alignment");
			n = (U32)v;
		}
		if(!expect(TokKind::RParen))
			return false;
		align = std::max(align, n);
		return true;
	}

	// [alignas]...
	B32 Parser::acceptTrailingAlignas(U32& align) {
		while(check(TokKind::KwAlignas))
			if(!parseAlignasSpec(align))
				return false;
		return true;
	}

	// records one qualifier or storage-class keyword
	void Parser::applyQualStorage(DeclSpecs& seen, TokKind kind) {
		if(detail::tokSyntax(kind).spec & detail::TokSyntax::Storage)
			++seen.storageCount;
		switch(kind) {
		case TokKind::KwStatic:
			seen.isStatic = true;
			break;
		case TokKind::KwExtern:
			seen.isExtern = true;
			break;
		case TokKind::KwInline:
			seen.isInline = true;
			break;
		case TokKind::KwConst:
			seen.isConst = true;
			break;
		case TokKind::KwVolatile:
			seen.isVolatile = true;
			break;
		case TokKind::KwNoinline:
			seen.isNoInline = true;
			break;
		default:
			break;
		}
	}

	// [ noinline | alignas ]...
	// then publishes the specs to the caller
	B32 Parser::finishTypeSpec(DeclSpecs seen, CType& out) {
		while(check(TokKind::KwAlignas) || check(TokKind::KwNoinline)) {
			if(accept(TokKind::KwNoinline))
				seen.isNoInline = true;
			else if(!parseAlignasSpec(seen.align))
				return false;
		}
		if(seen.isConst)
			setTopConst(out);
		if(seen.isVolatile)
			setTopVolatile(out);
		specs = seen;
		return true;
	}

	// [ qualifier | storage | alignas ]...
	B32 Parser::parseQualStorage(DeclSpecs& seen) {
		for(;;) {
			if(check(TokKind::KwAlignas)) {
				if(!parseAlignasSpec(seen.align))
					return false;
				continue;
			}
			if(!detail::isQualOrStorage(peek().kind))
				return true;
			applyQualStorage(seen, advance().kind);
		}
	}

	// type-keyword [ type-keyword | qualifier | storage | alignas ]...
	B32 Parser::parseTypeKeywords(DeclSpecs& seen, TypeWords& w) {
		for(;;) {
			if(!parseQualStorage(seen))
				return false;
			TokKind k = peek().kind;
			if(!(detail::tokSyntax(k).spec & detail::TokSyntax::TypeWord))
				return true;
			advance();
			++w.count[(U32)k];
			++w.total;
		}
	}

	CType Parser::basicType(const TypeWords& w) {
		B32 isFloat = w.of(TokKind::KwFloat) != 0;
		B32 isComplex = w.of(TokKind::KwComplex) || w.of(TokKind::KwImaginary);
		B32 isUnsigned = w.of(TokKind::KwUnsigned) != 0;
		U32 longCount = w.of(TokKind::KwLong);
		CType t;
		if(w.of(TokKind::KwVoid)) {
			t.base = CType::Base::Void;
		} else if(isFloat || w.of(TokKind::KwDouble) || isComplex) {
			t.base = CType::Base::Float;
			t.bits = 64;
			if(isFloat)
				t.bits = 32;
			else if(w.of(TokKind::KwDouble) && longCount >= 1)
				t.bits = 128;
			t.set(CType::Complex, isComplex);
			if(isComplex)
				t.strukt = complexLayout(t.bits);
		} else if(w.of(TokKind::KwBool)) {
			t = ctUnsigned(1);
		} else {
			t.set(CType::Unsigned, isUnsigned);
			if(w.of(TokKind::KwChar)) {
				t.bits = 8;
				t.set(CType::PlainChar, !isUnsigned && !w.of(TokKind::KwSigned));
			} else if(w.of(TokKind::KwShort))
				t.bits = 16;
			else if(longCount >= 2) {
				t.bits = 64;
				t.set(CType::Long);
				t.set(CType::LongLong);
			} else if(longCount == 1) {
				t.bits = lay.longBits; // 64 on LP64 linux, 32 on LLP64 windows
				t.set(CType::Long);
			} else
				t.bits = 32;
		}
		return t;
	}

	// [ qualifier | storage | alignas ]... spec-body [ noinline | alignas ]...
	// spec-body: typeof-spec | enum-spec | struct-spec | typedef-name
	//          | type-keyword [ type-keyword | qualifier | storage | alignas ]...
	B32 Parser::parseTypeSpec(CType& out) {
		DeclSpecs seen;
		specs = DeclSpecs{};
		if(!parseQualStorage(seen))
			return false;
		if(seen.storageCount > 1)
			return fail(peek(), "more than one storage-class specifier");
		if(check(TokKind::KwTypeof))
			return parseTypeofSpec(out) && finishTypeSpec(seen, out);
		if(check(TokKind::KwEnum))
			return parseEnumSpec(out) && finishTypeSpec(seen, out);
		if(check(TokKind::KwStruct) || check(TokKind::KwUnion))
			return parseStructSpec(out) && finishTypeSpec(seen, out);
		if(check(TokKind::Identifier)) {
			if(const CType* td = typedefType(lex.text(peek()))) {
				advance();
				out = *td;
				return finishTypeSpec(seen, out);
			}
		}
		TypeWords w;
		if(!parseTypeKeywords(seen, w) || w.total == 0)
			return false;
		if(seen.storageCount > 1)
			return fail(peek(), "more than one storage-class specifier");
		out = basicType(w);
		return finishTypeSpec(seen, out);
	}
} // namespace rat::cc
