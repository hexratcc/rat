#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	namespace detail {
		B32 isTypeQualifier(TokKind kind) {
			switch(kind) {
			case TokKind::KwConst:
			case TokKind::KwVolatile:
			case TokKind::KwRestrict:
			case TokKind::KwNoinline:
				return true;
			default:
				return false;
			}
		}

		B32 isQualOrStorage(TokKind kind) {
			switch(kind) {
			case TokKind::KwStatic:
			case TokKind::KwExtern:
			case TokKind::KwRegister:
			case TokKind::KwAuto:
			case TokKind::KwInline:
				return true;
			default:
				return isTypeQualifier(kind);
			}
		}

		B32 isTypeStart(TokKind kind) {
			switch(kind) {
			case TokKind::KwVoid:
			case TokKind::KwBool:
			case TokKind::KwChar:
			case TokKind::KwShort:
			case TokKind::KwInt:
			case TokKind::KwLong:
			case TokKind::KwFloat:
			case TokKind::KwDouble:
			case TokKind::KwSigned:
			case TokKind::KwUnsigned:
			case TokKind::KwComplex:
			case TokKind::KwImaginary:
			case TokKind::KwEnum:
			case TokKind::KwStruct:
			case TokKind::KwUnion:
			case TokKind::KwTypeof:
			case TokKind::KwAlignas:
				return true;
			default:
				return isQualOrStorage(kind);
			}
		}

		U64 alignUp(U64 value, U32 align) {
			return align <= 1 ? value : (value + align - 1) / align * align;
		}
	} // namespace detail

	// a type keyword, qualifier, storage class or typedef name
	B32 Parser::startsType(const Token& tok) {
		if(detail::isTypeStart(tok.kind))
			return true;
		return tok.kind == TokKind::Identifier && typedefs.get(lex.text(tok)) != nullptr;
	}

	// typedef type-spec [ declarator [, declarator]... ] ;
	B32 Parser::parseTypedef() {
		advance(); // typedef
		CType base;
		if(!parseTypeSpec(base)) {
			fail(peek(), "expected type in typedef declaration");
			return false;
		}
		if(accept(TokKind::Semicolon))
			return true;
		for(;;) {
			DeclResult r;
			if(!parseDeclarator(base, r))
				return false;
			if(!r.name) {
				fail(peek(), "expected typedef name");
				return false;
			}
			typedefs.set(*r.name, r.type);
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::Semicolon, "';'");
	}

	// _Alignas ( type-name | const-expr )
	// raises align, never lowers it
	B32 Parser::parseAlignasSpec(U32& align) {
		Token kw = advance(); // _Alignas
		if(!expect(TokKind::LParen, "'('"))
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
			if(v <= 0 || (v & (v - 1)) != 0) {
				fail(kw, "_Alignas requires a positive power-of-two alignment");
				return false;
			}
			n = (U32)v;
		}
		if(!expect(TokKind::RParen, "')'"))
			return false;
		if(n > align)
			align = n;
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
		switch(kind) {
		case TokKind::KwStatic:
			seen.isStatic = true;
			++seen.storageCount;
			break;
		case TokKind::KwExtern:
			seen.isExtern = true;
			++seen.storageCount;
			break;
		case TokKind::KwAuto:
		case TokKind::KwRegister:
			++seen.storageCount;
			break;
		case TokKind::KwInline:
			seen.isInline = true;
			break;
		case TokKind::KwConst:
			seen.isConst = true;
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
		for(;;) {
			if(check(TokKind::KwAlignas)) {
				if(!parseAlignasSpec(specAlign))
					return false;
				continue;
			}
			if(accept(TokKind::KwNoinline)) {
				seen.isNoInline = true;
				continue;
			}
			break;
		}
		if(seen.isConst)
			out.quals |= 1u;
		specs = seen;
		return true;
	}

	// [ qualifier | storage | alignas ]...
	B32 Parser::parseQualStorage(DeclSpecs& seen) {
		for(;;) {
			if(check(TokKind::KwAlignas)) {
				if(!parseAlignasSpec(specAlign))
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
		// clang-format off
		static const TokKind kWords[] = {
				TokKind::KwVoid, TokKind::KwBool, TokKind::KwChar, TokKind::KwShort, TokKind::KwInt,
				TokKind::KwLong, TokKind::KwFloat, TokKind::KwDouble, TokKind::KwSigned,
				TokKind::KwUnsigned, TokKind::KwComplex, TokKind::KwImaginary,
		};
		// clang-format on
		for(;;) {
			if(!parseQualStorage(seen))
				return false;
			TokKind k = peek().kind;
			if(std::find(std::begin(kWords), std::end(kWords), k) == std::end(kWords))
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
			t.bits = isFloat ? 32 : (w.of(TokKind::KwDouble) && longCount >= 1 ? 128 : 64);
			t.set(CType::Complex, isComplex);
			if(isComplex)
				t.strukt = complexStruct(t);
		} else if(w.of(TokKind::KwBool)) {
			t.bits = 1;
			t.set(CType::Unsigned);
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
		specAlign = 0;
		if(!parseQualStorage(seen))
			return false;
		if(seen.storageCount > 1) {
			fail(peek(), "more than one storage-class specifier");
			return false;
		}
		if(check(TokKind::KwTypeof))
			return parseTypeofSpec(out) && finishTypeSpec(seen, out);
		if(check(TokKind::KwEnum))
			return parseEnumSpec(out) && finishTypeSpec(seen, out);
		if(check(TokKind::KwStruct) || check(TokKind::KwUnion))
			return parseStructSpec(out) && finishTypeSpec(seen, out);
		if(check(TokKind::Identifier)) {
			if(const CType* td = typedefs.get(lex.text(peek()))) {
				advance();
				out = *td;
				return finishTypeSpec(seen, out);
			}
		}
		TypeWords w;
		if(!parseTypeKeywords(seen, w) || w.total == 0)
			return false;
		if(seen.storageCount > 1) {
			fail(peek(), "more than one storage-class specifier");
			return false;
		}
		out = basicType(w);
		return finishTypeSpec(seen, out);
	}
} // namespace rat::cc
