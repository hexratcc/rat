#include "Parse/Parser.h"

#include "Parse/ParserDetail.h"

namespace rat::cc {
	namespace detail {
		B32 isTypeQualifier(TokKind kind) {
			switch(kind) {
			case TokKind::KwConst:
			case TokKind::KwVolatile:
			case TokKind::KwRestrict:
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
				return true;
			default:
				return isQualOrStorage(kind);
			}
		}

		U32 alignUp(U32 value, U32 align) {
			return align <= 1 ? value : (value + align - 1) / align * align;
		}
	} // namespace detail

	B32 Parser::startsType(const Token& tok) {
		if(detail::isTypeStart(tok.kind))
			return true;
		return tok.kind == TokKind::Identifier && typedefs.count(lex.text(tok)) != 0;
	}

	B32 Parser::parseTypedef() {
		advance(); // typedef
		CType base;
		if(!parseTypeSpec(base)) {
			fail(peek(), "expected type in typedef declaration");
			return false;
		}
		if(peek().kind == TokKind::Semicolon) {
			advance();
			return true;
		}
		for(;;) {
			Token nameTok;
			B32 haveName = false;
			CType t;
			if(!parseDeclaratorType(base, nameTok, haveName, t))
				return false;
			if(!haveName) {
				fail(peek(), "expected typedef name");
				return false;
			}
			typedefs[lex.text(nameTok)] = t;
			if(!accept(TokKind::Comma))
				break;
		}
		return expect(TokKind::Semicolon, "';'");
	}

	void Parser::parsePointers(CType& t) {
		for(;;) {
			B32 sawConst = false;
			while(detail::isTypeQualifier(peek().kind)) {
				if(peek().kind == TokKind::KwConst)
					sawConst = true;
				advance();
			}
			if(sawConst && t.ptr < 32)
				setTopConst(t);
			if(!accept(TokKind::Star))
				break;
			++t.ptr;
		}
	}

	B32 Parser::parseTypeSpec(CType& out) {
		B32 isStatic = false;
		B32 isExtern = false;
		B32 isInline = false;
		B32 isConst = false;
		I32 storageCount = 0;
		sawStatic = false;
		sawExtern = false;
		sawInline = false;
		auto applyQualStorage = [&](TokKind sk) {
			if(sk == TokKind::KwStatic)
				isStatic = true;
			if(sk == TokKind::KwExtern)
				isExtern = true;
			if(sk == TokKind::KwInline)
				isInline = true;
			if(sk == TokKind::KwConst)
				isConst = true;
			if(sk == TokKind::KwStatic || sk == TokKind::KwExtern || sk == TokKind::KwAuto ||
				 sk == TokKind::KwRegister)
				++storageCount;
		};
		while(detail::isQualOrStorage(peek().kind)) {
			applyQualStorage(peek().kind);
			advance();
		}
		auto finishSpec = [&] {
			if(isConst)
				out.quals |= 1u;
			setStorage(isStatic, isExtern, isInline);
		};
		if(storageCount > 1) {
			fail(peek(), "more than one storage-class specifier");
			return false;
		}
		if(peek().kind == TokKind::KwTypeof) {
			B32 ok = parseTypeofSpec(out);
			finishSpec();
			return ok;
		}
		if(peek().kind == TokKind::KwEnum) {
			B32 ok = parseEnumSpec(out);
			finishSpec();
			return ok;
		}
		if(peek().kind == TokKind::KwStruct || peek().kind == TokKind::KwUnion) {
			B32 ok = parseStructSpec(out);
			finishSpec();
			return ok;
		}
		if(peek().kind == TokKind::Identifier) {
			auto it = typedefs.find(lex.text(peek()));
			if(it != typedefs.end()) {
				advance();
				out = it->second;
				finishSpec();
				return true;
			}
		}

		B32 isVoid = false, isBool = false, isChar = false, isShort = false;
		B32 isUnsigned = false, isSigned = false;
		B32 isFloat = false, isDouble = false;
		B32 isComplex = false;
		I32 longCount = 0;
		I32 count = 0;
		for(;;) {
			TokKind k = peek().kind;
			if(k == TokKind::KwVoid)
				isVoid = true;
			else if(k == TokKind::KwBool)
				isBool = true;
			else if(k == TokKind::KwChar)
				isChar = true;
			else if(k == TokKind::KwShort)
				isShort = true;
			else if(k == TokKind::KwFloat)
				isFloat = true;
			else if(k == TokKind::KwDouble)
				isDouble = true;
			else if(k == TokKind::KwComplex || k == TokKind::KwImaginary)
				isComplex = true;
			else if(k == TokKind::KwLong)
				++longCount;
			else if(k == TokKind::KwUnsigned)
				isUnsigned = true;
			else if(k == TokKind::KwSigned)
				isSigned = true;
			else if(k == TokKind::KwInt)
				; // base int
			else if(detail::isQualOrStorage(k)) {
				applyQualStorage(k);
				advance();
				continue;
			} else
				break;
			advance();
			++count;
		}
		if(count == 0)
			return false;
		if(storageCount > 1) {
			fail(peek(), "more than one storage-class specifier");
			return false;
		}
		CType t;
		if(isVoid) {
			t.base = CType::Base::Void;
		} else if(isFloat || isDouble || isComplex) {
			t.base = CType::Base::Float;
			t.bits = isFloat ? 32 : (isDouble && longCount >= 1 ? 128 : 64);
			t.set(CType::Complex, isComplex);
			if(isComplex)
				t.strukt = complexStruct(t);
		} else if(isBool) {
			t.bits = 1;
			t.set(CType::Unsigned);
		} else {
			t.set(CType::Unsigned, isUnsigned);
			if(isChar) {
				t.bits = 8;
				t.set(CType::PlainChar, !isUnsigned && !isSigned);
			} else if(isShort)
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
		if(isConst)
			t.quals |= 1u;
		out = t;
		setStorage(isStatic, isExtern, isInline);
		return true;
	}
} // namespace rat::cc
