#include "Parse/Parser.h"

#include "Parse/ParserDetail.h"

namespace rat::cc {
	StructType* Parser::complexStruct(CType realType) {
		StructType*& st = complexLayouts[realType.bits];
		if(!st)
			st = makeComplexLayout(arena, realType);
		return st;
	}

	B32 Parser::parseStructBody(StructType* st, B32 isUnion) {
		U32 offset = 0;
		U32 align = 1;
		U32 bitUnitOffset = 0;
		U32 bitUnitBytes = 0;
		U32 bitUnitUsed = 0;
		while(peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
			CType base;
			if(!parseTypeSpec(base)) {
				fail(peek(), "expected member type");
				return false;
			}
			if(peek().kind == TokKind::Semicolon && isStruct(base)) {
				advance();
				const StructType* inner = base.strukt;
				U32 mAlign = inner->align;
				U32 mbase = isUnion ? 0 : detail::alignUp(offset, mAlign);
				B32 first = true;
				for(const Field& sub : inner->fields) {
					Field f = sub;
					f.offset = isUnion ? sub.offset : mbase + sub.offset;
					f.set(Field::AnonMember);
					f.set(Field::AnonFirst, first);
					f.set(Field::AnonUnion, inner->isUnion);
					first = false;
					st->fields.push_back(f);
				}
				U32 mEnd = mbase + inner->size;
				if(isUnion) {
					if(inner->size > offset)
						offset = inner->size;
				} else {
					offset = mEnd;
				}
				if(mAlign > align)
					align = mAlign;
				bitUnitBytes = 0;
				continue;
			}
			for(;;) {
				CType ft = base;
				parsePointers(ft);
				Token nameTok;
				B32 haveName = false;
				B32 isArr = false;
				B32 flexible = false;
				U32 count = 0;
				if(looksLikeFuncPtr()) {
					CType fpt;
					if(!parseFuncPtrDeclarator(ft, nameTok, fpt))
						return false;
					ft = fpt;
					haveName = true;
				} else {
					if(peek().kind == TokKind::Identifier) {
						nameTok = advance();
						haveName = true;
					}
					if(peek().kind == TokKind::LBracket) {
						Declarator d;
						d.type = ft;
						if(!parseArraySuffix(d))
							return false;
						ft = d.type;
						isArr = true;
						I64 n = 0;
						if(d.arrayLen) {
							if(!evalIntConst(d.arrayLen, n) || n < 0) {
								fail(nameTok, "array member size must be a constant");
								return false;
							}
						} else {
							flexible = true; // type name[] with no bound
						}
						count = (U32)n; // 0 == flexible array member
					}
				}
				// optional bitfield : width
				if(accept(TokKind::Colon)) {
					if(isArr || ft.ptr != 0 || ft.isFloat() || ft.isComplex() || ft.isVoid() || isStruct(ft) ||
						 ft.func != nullptr) {
						fail(nameTok, "bit-field has invalid type");
						return false;
					}
					Expr* wE = parseConditional();
					if(!wE)
						return false;
					I64 w = 0;
					if(!evalIntConst(wE, w) || w < 0) {
						fail(peek(), "bitfield width must be a non-negative constant");
						return false;
					}
					U32 unitBytes = fieldByteSize(ft);
					if(unitBytes == 0)
						unitBytes = 4;
					U32 falign = fieldAlign(ft);
					if(w == 0 || bitUnitBytes != unitBytes || bitUnitUsed + (U32)w > unitBytes * 8) {
						bitUnitOffset = isUnion ? 0 : detail::alignUp(offset, falign);
						bitUnitBytes = unitBytes;
						bitUnitUsed = 0;
						if(!isUnion)
							offset = bitUnitOffset + unitBytes;
						else if(unitBytes > offset)
							offset = unitBytes;
					}
					if(w > 0 && haveName) {
						Field f;
						f.name = arena.make<String>(lex.text(nameTok));
						f.type = ft;
						f.set(Field::Bitfield);
						f.bitWidth = (U32)w;
						f.bitOffset = bitUnitUsed;
						f.offset = bitUnitOffset;
						st->fields.push_back(f);
					}
					bitUnitUsed += (U32)w;
					if(falign > align)
						align = falign;
					if(!accept(TokKind::Comma))
						break;
					continue;
				}
				bitUnitBytes = 0;
				if(!haveName) {
					fail(peek(), "expected member name");
					return false;
				}
				if(isStruct(ft) && !ft.strukt->complete) {
					fail(nameTok, "member has incomplete struct type");
					return false;
				}
				if(flexible) {
					if(isUnion) {
						fail(nameTok, "flexible array member not allowed in union");
						return false;
					}
					if(st->fields.empty()) {
						fail(nameTok, "flexible array member in struct with no other members");
						return false;
					}
					if(peek().kind == TokKind::Comma ||
						 !(peek().kind == TokKind::Semicolon && peek2().kind == TokKind::RBrace)) {
						fail(nameTok, "flexible array member must be the last member");
						return false;
					}
				}
				U32 esize = fieldByteSize(ft);
				U32 falign = fieldAlign(ft);
				U32 fsize = isArr ? esize * count : esize;
				Field f;
				f.name = arena.make<String>(lex.text(nameTok));
				f.type = ft;
				f.set(Field::Array, isArr);
				f.count = count;
				f.offset = isUnion ? 0 : detail::alignUp(offset, falign);
				st->fields.push_back(f);
				if(isUnion) {
					if(fsize > offset)
						offset = fsize;
				} else {
					offset = f.offset + fsize;
				}
				if(falign > align)
					align = falign;
				if(!accept(TokKind::Comma))
					break;
			}
			if(!expect(TokKind::Semicolon, "';'"))
				return false;
		}
		if(!expect(TokKind::RBrace, "'}'"))
			return false;
		st->align = align;
		st->size = detail::alignUp(offset, align);
		st->complete = true;
		return true;
	}

	B32 Parser::parseStructSpec(CType& out) {
		B32 isUnion = peek().kind == TokKind::KwUnion;
		advance(); // 'struct' or 'union'

		const String* tag = nullptr;
		if(peek().kind == TokKind::Identifier)
			tag = arena.make<String>(lex.text(advance()));

		StructType* st = nullptr;
		if(tag) {
			auto it = structTypes.find(*tag);
			if(it != structTypes.end())
				st = it->second;
			else {
				st = arena.make<StructType>();
				st->tag = *tag;
				st->isUnion = isUnion;
				structTypes[*tag] = st;
			}
		}

		if(peek().kind == TokKind::LBrace) {
			advance();
			if(!st)
				st = arena.make<StructType>(); // anon aggregate
			st->isUnion = isUnion;
			if(!parseStructBody(st, isUnion))
				return false;
		}

		if(!st) {
			fail(peek(), "use of undeclared struct/union tag");
			return false;
		}
		CType t;
		t.strukt = st;
		out = t;
		return true;
	}

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

	B32 Parser::parseEnumSpec(CType& out) {
		advance(); // enum
		if(peek().kind == TokKind::Identifier)
			advance();

		B32 anyNegative = false;
		if(peek().kind == TokKind::LBrace) {
			advance();
			I64 next = 0;
			while(peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
				if(peek().kind != TokKind::Identifier) {
					fail(peek(), "expected enumerator name");
					return false;
				}
				Token name = advance();
				I64 value = next;
				if(accept(TokKind::Assign)) {
					Expr* init = parseConditional();
					if(!init)
						return false;
					if(!evalIntConst(init, value))
						return false;
				}
				if(value < detail::kIntMin || value > detail::kIntMax) {
					fail(name, "enumerator value is not representable as int");
					return false;
				}
				if(value < 0)
					anyNegative = true;
				enumConstants[lex.text(name)] = value;
				next = value + 1;
				if(!accept(TokKind::Comma))
					break;
			}
			if(!expect(TokKind::RBrace, "'}'"))
				return false;
		}
		out = ctInt();
		if(!anyNegative)
			out.set(CType::Unsigned);
		return true;
	}
} // namespace rat::cc
