#include "Parse/Parser.h"

#include "Lex/CharClass.h"
#include "Parse/ParserDetail.h"

namespace rat::cc {
	namespace detail {
		void utf8Encode(String& out, U32 cp) {
			if(cp < 0x80) {
				out.push_back((char)cp);
			} else if(cp < 0x800) {
				out.push_back((char)(0xC0 | (cp >> 6)));
				out.push_back((char)(0x80 | (cp & 0x3F)));
			} else if(cp < 0x10000) {
				out.push_back((char)(0xE0 | (cp >> 12)));
				out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (cp & 0x3F)));
			} else {
				out.push_back((char)(0xF0 | (cp >> 18)));
				out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
				out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (cp & 0x3F)));
			}
		}
	} // namespace detail

	B32 Parser::parseIntLiteral(const Token& tok, I64& value, U32& bits, U8& mods) {
		String s = lex.text(tok);
		B32 isUnsigned = false;
		B32 isLong = false;
		B32 isLongLong = false;
		U32 lCount = 0;

		U32 end = (U32)s.size();
		while(end > 0) {
			char c = s[end - 1];
			if(c == 'u' || c == 'U') {
				isUnsigned = true;
				--end;
			} else if(c == 'l' || c == 'L') {
				++lCount;
				--end;
			} else {
				break;
			}
		}

		I32 base = 10;
		U32 start = 0;
		if(end >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			start = 2;
		} else if(end >= 1 && s[0] == '0') {
			base = 8;
		}

		U64 v = 0;
		for(U32 i = start; i < end; ++i) {
			I32 d = hexVal(s[i]);
			if(d < 0 || d >= base) {
				fail(tok, "invalid digit in integer constant");
				return false;
			}
			v = v * (U64)base + (U64)d;
		}

		B32 dec = (base == 10);
		B32 fitsI32 = v <= 0x7fffffffULL;
		B32 fitsU32 = v <= 0xffffffffULL;
		B32 fitsI64 = v <= 0x7fffffffffffffffULL;
		isLong = lCount >= 1;
		isLongLong = lCount >= 2;
		U64 longMax = lay.longBits >= 64 ? 0x7fffffffffffffffULL : 0x7fffffffULL;
		U64 ulongMax = lay.longBits >= 64 ? 0xffffffffffffffffULL : 0xffffffffULL;
		B32 fitsLong = v <= longMax;
		B32 fitsULong = v <= ulongMax;
		if(isUnsigned) {
			if(!isLongLong && !fitsULong)
				isLongLong = true;
			else if(!isLong && !fitsU32)
				isLong = true;
		} else if(isLongLong) {
			if(!fitsI64)
				isUnsigned = true;
		} else if(isLong) {
			if(!fitsLong && dec)
				isLongLong = true;
			else if(!fitsLong && fitsULong)
				isUnsigned = true;
			else if(!fitsI64)
				isLongLong = true;
		} else if(dec) {
			if(!fitsI32 && fitsLong)
				isLong = true;
			else if(!fitsI32)
				isLongLong = true;
		} else {
			// hex/octal: int, unsigned int, long, unsigned long, long long, unsigned long long
			if(fitsI32) {
				// int
			} else if(fitsU32) {
				isUnsigned = true;
			} else if(fitsLong) {
				isLong = true;
			} else if(fitsULong) {
				isLong = true;
				isUnsigned = true;
			} else if(fitsI64) {
				isLongLong = true;
			} else {
				isLongLong = true;
				isUnsigned = true;
			}
		}

		bits = (isLongLong || (isLong && lay.longBits >= 64)) ? 64 : (isLong ? lay.longBits : 32);
		value = (I64)v;
		mods = (U8)((isUnsigned ? CType::Unsigned : 0) | (isLong ? CType::Long : 0) |
								(isLongLong ? CType::LongLong : 0));
		return true;
	}

	static U32 escapeMaxVal(char prefix) {
		switch(prefix) {
		case 'u':
			return 0xFFFFu;
		case 'L':
		case 'U':
			return 0xFFFFFFFFu;
		default:
			return 0xFFu;
		}
	}

	B32 Parser::decodeEscape(
			const String& s, U32& i, U32 end, const Token& tok, U32 maxVal, U8& out) {
		if(i >= end) {
			fail(tok, "unterminated escape");
			return false;
		}
		char e = s[i++];
		if(simpleEscape(e, out))
			return true;
		switch(e) {
		case '?':
			out = '?';
			return true;
		case 'x': {
			if(i >= end || !isHexDigit(s[i])) {
				fail(tok, "expected hex digits in escape");
				return false;
			}
			U32 hv = 0;
			while(i < end && isHexDigit(s[i]))
				hv = hv * 16 + (U32)hexVal(s[i++]);
			if(hv > maxVal) {
				fail(tok, "hex escape out of range");
				return false;
			}
			out = (U8)hv;
			return true;
		}
		default:
			if(isOctalDigit(e)) {
				// octal escape
				U32 ov = (U32)(e - '0');
				for(U32 k = 0; k < 2 && i < end && isOctalDigit(s[i]); ++k, ++i)
					ov = ov * 8 + (U32)(s[i] - '0');
				if(ov > maxVal) {
					fail(tok, "octal escape out of range");
					return false;
				}
				out = (U8)ov;
			} else {
				out = (U8)e; // unknown escape: take the character literally
			}
			return true;
		}
	}

	B32 Parser::decodeUcn(const String& s, U32& i, U32 end, const Token& tok, U32& cp) {
		char kind = s[i++]; // 'u' or 'U'
		U32 ndigits = (kind == 'u') ? 4 : 8;
		if(i + ndigits > end) {
			fail(tok, "incomplete universal character name");
			return false;
		}
		U32 v = 0;
		for(U32 k = 0; k < ndigits; ++k) {
			if(!isHexDigit(s[i + k])) {
				fail(tok, "universal character name requires hex digits");
				return false;
			}
			v = v * 16 + (U32)hexVal(s[i + k]);
		}
		i += ndigits;
		if((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
			fail(tok, "invalid universal character name");
			return false;
		}
		cp = v;
		return true;
	}

	B32 Parser::parseCharLiteral(const Token& tok, I64& value) {
		String s = lex.text(tok);
		U32 maxVal = escapeMaxVal(s.size() ? s[0] : '\'');
		// skip any encoding prefix
		U32 i = 0;
		while(i < s.size() && s[i] != '\'')
			++i;
		++i;
		U32 end = (U32)s.size();
		if(end > 0 && s[end - 1] == '\'')
			--end;

		if(i >= end) {
			fail(tok, "empty character constant");
			return false;
		}

		I64 v = 0;
		U32 count = 0;
		while(i < end) {
			I64 c;
			if(s[i] == '\\') {
				++i;
				if(i < end && (s[i] == 'u' || s[i] == 'U')) {
					U32 cp;
					if(!decodeUcn(s, i, end, tok, cp))
						return false;
					c = (I64)cp;
				} else {
					U8 byte;
					if(!decodeEscape(s, i, end, tok, maxVal, byte))
						return false;
					c = (I64)(I8)byte;
				}
			} else {
				c = (U8)s[i++];
			}
			if(count == 0)
				v = c;
			else
				v = (v << 8) | (c & 0xff);
			++count;
		}
		value = v;
		return true;
	}

	B32 Parser::parseStringLiteral(const Token& tok, String& out) {
		String s = lex.text(tok);
		U32 maxVal = (s.size() && s[0] == 'u' && s.size() > 1 && s[1] == '8')
										 ? 0xFFu
										 : escapeMaxVal(s.size() ? s[0] : '"');
		U32 i = 0;
		while(i < s.size() && s[i] != '"')
			++i;
		++i;
		U32 end = (U32)s.size();
		if(end > 0 && s[end - 1] == '"')
			--end;
		while(i < end) {
			char c = s[i++];
			if(c != '\\') {
				out.push_back(c);
				continue;
			}
			if(i < end && (s[i] == 'u' || s[i] == 'U')) {
				U32 cp;
				if(!decodeUcn(s, i, end, tok, cp))
					return false;
				detail::utf8Encode(out, cp);
				continue;
			}
			U8 byte;
			if(!decodeEscape(s, i, end, tok, maxVal, byte))
				return false;
			out.push_back((char)byte);
		}
		return true;
	}

} // namespace rat::cc
