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

	// [ noinline | alignas ]...
	// then publishes the specs to the caller
	B32 Parser::finishTypeSpec(DeclSpecs seen, CType& out) {
		while(check(TokKind::KwAlignas) || check(TokKind::KwNoinline)) {
			if(accept(TokKind::KwNoinline))
				++seen.count[(U32)TokKind::KwNoinline];
			else if(!parseAlignasSpec(seen.align))
				return false;
		}
		if(seen.has(TokKind::KwConst))
			setTopConst(out);
		if(seen.has(TokKind::KwVolatile))
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
			TokKind kind = advance().kind;
			++seen.count[(U32)kind];
			if(detail::tokSyntax(kind).spec & detail::TokSyntax::Storage)
				++seen.storageCount;
		}
	}

	// type-keyword [ type-keyword | qualifier | storage | alignas ]...
	B32 Parser::parseTypeKeywords(DeclSpecs& seen) {
		for(;;) {
			if(!parseQualStorage(seen))
				return false;
			TokKind k = peek().kind;
			if(!(detail::tokSyntax(k).spec & detail::TokSyntax::TypeWord))
				return true;
			advance();
			++seen.count[(U32)k];
			++seen.typeWordCount;
		}
	}

	CType Parser::basicType(const DeclSpecs& seen) {
		B32 isFloat = seen.of(TokKind::KwFloat) != 0;
		B32 isComplex = seen.of(TokKind::KwComplex) || seen.of(TokKind::KwImaginary);
		B32 isUnsigned = seen.of(TokKind::KwUnsigned) != 0;
		U32 longCount = seen.of(TokKind::KwLong);
		CType t;
		if(seen.of(TokKind::KwVoid)) {
			t.base = CType::Base::Void;
		} else if(isFloat || seen.of(TokKind::KwDouble) || isComplex) {
			U32 bits = 64;
			if(isFloat)
				bits = 32;
			else if(seen.of(TokKind::KwDouble) && longCount >= 1)
				bits = 128;
			t = ctFloat(bits, isComplex);
		} else if(seen.of(TokKind::KwBool)) {
			t = ctUnsigned(1);
		} else {
			t.set(CType::Unsigned, isUnsigned);
			if(seen.of(TokKind::KwChar)) {
				t.bits = 8;
				t.set(CType::PlainChar, !isUnsigned && !seen.of(TokKind::KwSigned));
			} else if(seen.of(TokKind::KwShort))
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
		if(!parseTypeKeywords(seen) || seen.typeWordCount == 0)
			return false;
		if(seen.storageCount > 1)
			return fail(peek(), "more than one storage-class specifier");
		out = basicType(seen);
		return finishTypeSpec(seen, out);
	}
} // namespace rat::cc
