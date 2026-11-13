#include "parse/parser.h"

#include "parse/parser_detail.h"

namespace rat::cc {
	// one shared { re, im } layout per element width
	StructType* Parser::complexStruct(CType realType) {
		StructType*& st = complexLayouts[realType.bits];
		if(!st)
			st = makeComplexLayout(arena, realType);
		return st;
	}

	// the next slot that fits align; a union starts every member at 0
	U64 Parser::placeMember(U64 size, U32 align, B32 isUnion, StructLayout& l) {
		U64 at = isUnion ? 0 : detail::alignUp(l.offset, align);
		if(isUnion) {
			if(size > l.offset)
				l.offset = size;
		} else {
			l.offset = at + size;
		}
		l.bitPos = l.offset * 8;
		if(align > l.align)
			l.align = align;
		return at;
	}

	// the members of an unnamed struct or union member become members of st
	void
	Parser::spliceAnonMember(StructType* st, const StructType* inner, B32 isUnion, StructLayout& l) {
		U64 mbase = placeMember(inner->size, inner->align, isUnion, l);
		B32 first = true;
		for(const Field& sub : inner->fields) {
			Field f = sub;
			f.offset = mbase + sub.offset;
			f.set(Field::AnonMember);
			f.set(Field::AnonFirst, first);
			f.set(Field::AnonUnion, inner->isUnion);
			first = false;
			st->fields.push_back(f);
		}
	}

	// array members need a constant, non-negative bound on every level
	B32 Parser::arrayMemberCount(CType t, U64& count) {
		count = t.array->count;
		if(!t.array->countExpr && !hasVlaDim(t.array->elem))
			return true;
		I64 n = 0;
		if(hasVlaDim(t.array->elem) || !evalIntConst(t.array->countExpr, n) || n < 0)
			return fail(peek(), "array member size must be a constant");
		count = (U64)n;
		return true;
	}

	// const-expr after the ':' of a member
	// an unnamed or zero-width bitfield only takes space; the unit is the member's own type
	B32 Parser::parseBitfield(
			StructType* st, Field f, U32 memberAlign, B32 isUnion, StructLayout& l) {
		Expr* wE = parseConditional();
		if(!wE)
			return false;
		I64 w = 0;
		if(!evalIntConst(wE, w) || w < 0)
			return fail(peek(), "bitfield width must be a non-negative constant");
		U32 unitBytes = typeSizeBytes(f.type);
		if(unitBytes == 0)
			unitBytes = 4;
		U32 falign = typeAlignBytes(f.type);
		if(memberAlign > falign)
			falign = memberAlign;
		U32 unitBits = unitBytes * 8;
		U32 unitStart = 0;
		if(isUnion) {
			if(unitBytes > l.offset)
				l.offset = unitBytes;
		} else {
			if(w == 0 || l.bitPos % unitBits + (U32)w > unitBits)
				l.bitPos = detail::alignUp(l.bitPos, unitBits);
			unitStart = l.bitPos / unitBits * unitBytes;
		}
		if(w > 0 && f.name) {
			f.type.bitPrec = (U8)w; // the field's values wrap at this width
			f.set(Field::Bitfield);
			f.bitWidth = (U32)w;
			f.bitOffset = isUnion ? 0 : l.bitPos % unitBits;
			f.offset = unitStart;
			st->fields.push_back(f);
		}
		if(isUnion) {
			l.bitPos = l.offset * 8;
		} else {
			l.bitPos += (U32)w;
			l.offset = (l.bitPos + 7) / 8;
		}
		if(falign > l.align)
			l.align = falign;
		return true;
	}

	// declarator [ : bitfield ]
	// only a bitfield may be unnamed; an array with no bound at all is a flexible array member
	B32 Parser::parseStructMember(
			StructType* st, CType base, U32 baseAlign, B32 isUnion, StructLayout& l) {
		DeclResult r;
		r.align = baseAlign;
		if(!parseDeclarator(base, r))
			return false;
		CType ft = r.type;
		B32 isArr = isArrayType(ft);
		U64 count = 0;
		if(isArr) {
			if(!arrayMemberCount(ft, count))
				return false;
			ft = ft.array->elem;
		}
		Field f;
		f.name = r.name;
		f.type = ft;
		if(accept(TokKind::Colon)) {
			if(isArr || ft.ptr != 0 || ft.isFloat() || ft.isComplex() || ft.isVoid() || isStruct(ft) ||
				 ft.func != nullptr) {
				return fail(peek(), "bit-field has invalid type");
			}
			return parseBitfield(st, f, r.align, isUnion, l);
		}
		if(!r.name)
			return fail(peek(), "expected member name");
		if(isStruct(ft) && !ft.strukt->complete)
			return fail(peek(), "member has incomplete struct type");
		if(isArr && count == 0 && !r.outerBound) {
			if(isUnion)
				return fail(peek(), "flexible array member not allowed in union");
			if(st->fields.empty())
				return fail(peek(), "flexible array member in struct with no other members");
			if(!(check(TokKind::Semicolon) && peek2().kind == TokKind::RBrace))
				return fail(peek(), "flexible array member must be the last member");
		}
		f.set(Field::Array, isArr);
		f.count = count;
		U64 size = typeSizeBytes(ft);
		if(isArr)
			size *= count;
		U32 falign = typeAlignBytes(ft);
		if(r.align > falign)
			falign = r.align;
		f.offset = placeMember(size, falign, isUnion, l);
		st->fields.push_back(f);
		return true;
	}

	// [ struct-spec ; | type-spec member [, member]... ; ]... }
	// a bare struct-spec ; splices an anonymous struct or union member
	B32 Parser::parseStructBody(StructType* st, B32 isUnion) {
		StructLayout l;
		while(!check(TokKind::RBrace) && !check(TokKind::Eof)) {
			CType base;
			if(!parseTypeSpec(base))
				return fail(peek(), "expected member type");
			U32 baseAlign = specAlign;
			if(isStruct(base) && accept(TokKind::Semicolon)) {
				spliceAnonMember(st, base.strukt, isUnion, l);
				continue;
			}
			for(;;) {
				if(!parseStructMember(st, base, baseAlign, isUnion, l))
					return false;
				if(!accept(TokKind::Comma))
					break;
			}
			if(!expect(TokKind::Semicolon, "';'"))
				return false;
		}
		if(!expect(TokKind::RBrace, "'}'"))
			return false;
		st->align = l.align;
		st->size = detail::alignUp(l.offset, l.align);
		st->complete = true;
		return true;
	}

	// tag-kw [alignas]... [tag] [ { body ] [alignas]...
	// tag-kw: struct | union
	B32 Parser::parseStructSpec(CType& out) {
		B32 isUnion = check(TokKind::KwUnion);
		advance(); // 'struct' or 'union'

		U32 declAlign = 0;
		if(!acceptTrailingAlignas(declAlign))
			return false;

		const String* tag = nullptr;
		if(check(TokKind::Identifier))
			tag = arena.make<String>(lex.text(advance()));

		B32 hasBody = check(TokKind::LBrace);
		StructType* st = nullptr;
		if(tag) {
			const TagBinding* bound = structTypes.get(*tag);
			if(bound && !(hasBody && bound->depth < scopeDepth))
				st = bound->type;
			else {
				st = arena.make<StructType>();
				st->tag = *tag;
				st->isUnion = isUnion;
				structTypes.set(*tag, TagBinding{st, scopeDepth});
			}
		}

		if(hasBody) {
			advance(); // {
			if(!st)
				st = arena.make<StructType>(); // anon aggregate
			st->isUnion = isUnion;
			if(!parseStructBody(st, isUnion))
				return false;
		}

		if(!st)
			return fail(peek(), "use of undeclared struct/union tag");
		if(!acceptTrailingAlignas(declAlign))
			return false;
		if(declAlign > st->align) {
			st->align = declAlign;
			st->size = detail::alignUp(st->size, declAlign);
		}
		CType t;
		t.strukt = st;
		out = t;
		return true;
	}

	// typeof ( type-name | expr )
	B32 Parser::parseTypeofSpec(CType& out) {
		advance(); // typeof / __typeof / __typeof__
		if(!expect(TokKind::LParen, "'('"))
			return false;
		if(startsType(peek())) {
			if(!parseTypeName(out))
				return false;
		} else {
			Expr* e = parseExpression();
			if(!e)
				return false;
			out = CType{};
			out.typeofExpr = e;
		}
		return expect(TokKind::RParen, "')'");
	}

	// [ enumerator [, enumerator]... [,] ] }
	// enumerator: name [= const-expr]
	B32 Parser::parseEnumerators(NamedType& e) {
		I64 next = 0;
		B32 anyNegative = false;
		while(!check(TokKind::RBrace) && !check(TokKind::Eof)) {
			if(!check(TokKind::Identifier))
				return fail(peek(), "expected enumerator name");
			Token name = advance();
			I64 value = next;
			if(accept(TokKind::Assign)) {
				Expr* init = parseConditional();
				if(!init)
					return false;
				if(!evalIntConst(init, value))
					return false;
			}
			if(value < detail::kIntMin || value > detail::kIntMax)
				return fail(name, "enumerator value is not representable as int");
			if(value < 0)
				anyNegative = true;
			enumConstants.set(lex.text(name), value);
			e.enumerators.push_back({lex.text(name), value});
			next = value + 1;
			if(!accept(TokKind::Comma))
				break;
		}
		e.type.set(CType::Unsigned, !anyNegative);
		return expect(TokKind::RBrace, "'}'");
	}

	// enum [alignas]... [tag] [ { enumerators ]
	B32 Parser::parseEnumSpec(CType& out) {
		Token kw = advance(); // enum
		String tag;
		U32 ignored = 0; // an enum's alignment is its underlying type's
		if(!acceptTrailingAlignas(ignored))
			return false;
		if(check(TokKind::Identifier))
			tag = lex.text(advance());

		const NamedType* e = nullptr;
		if(accept(TokKind::LBrace)) {
			NamedType* defined = arena.make<NamedType>(NamedType{tag, ctInt(), {}, kw.offset, true});
			if(!parseEnumerators(*defined))
				return false;
			if(!tag.empty())
				enumTags.set(tag, defined);
			e = defined;
		} else if(const NamedType* const* seen = enumTags.get(tag)) {
			e = *seen;
		}
		out = e ? e->type : ctInt();
		out.named = e;
		if(!e)
			out.set(CType::Unsigned);
		return true;
	}
} // namespace rat::cc
