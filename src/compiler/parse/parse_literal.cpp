#include "parse/parser.h"

#include "lex/char_class.h"
#include "parse/parser_detail.h"

namespace rat::cc {
	namespace detail {
		void utf8Encode(String& out, U32 cp) {
			if(cp < 0x80) {
				out.push_back((C8)cp);
			} else if(cp < 0x800) {
				out.push_back((C8)(0xC0 | (cp >> 6)));
				out.push_back((C8)(0x80 | (cp & 0x3F)));
			} else if(cp < 0x10000) {
				out.push_back((C8)(0xE0 | (cp >> 12)));
				out.push_back((C8)(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back((C8)(0x80 | (cp & 0x3F)));
			} else {
				out.push_back((C8)(0xF0 | (cp >> 18)));
				out.push_back((C8)(0x80 | ((cp >> 12) & 0x3F)));
				out.push_back((C8)(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back((C8)(0x80 | (cp & 0x3F)));
			}
		}

		U32 utf8Decode(const String& bytes, U32& i, U32 n) {
			U8 b0 = (U8)bytes[i++];
			U32 cp = b0;
			U32 extra = 0;
			if((b0 & 0xE0) == 0xC0) {
				cp = b0 & 0x1F;
				extra = 1;
			} else if((b0 & 0xF0) == 0xE0) {
				cp = b0 & 0x0F;
				extra = 2;
			} else if((b0 & 0xF8) == 0xF0) {
				cp = b0 & 0x07;
				extra = 3;
			}
			for(U32 k = 0; k < extra && i < n; ++k, ++i)
				cp = (cp << 6) | ((U8)bytes[i] & 0x3F);
			return cp;
		}

		// largest escape value the encoding prefix of a literal can hold
		U32 escapeMaxVal(C8 prefix) {
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

		B32 ucnAt(const String& s, U32 i, U32 end) { return i < end && (s[i] == 'u' || s[i] == 'U'); }

		Pair<U32, U32> quotedRange(const String& s, C8 quote) {
			U32 begin = 0; // skip any encoding prefix
			while(begin < s.size() && s[begin] != quote)
				++begin;
			++begin;
			U32 end = (U32)s.size();
			if(end > 0 && s[end - 1] == quote)
				--end;
			return {begin, end};
		}

		void appendCodeUnits(String& out, U32 cp, U32 unitBytes) {
			if(unitBytes == 2 && cp >= 0x10000) { // UTF-16 surrogate pair
				U32 v = cp - 0x10000;
				appendCodeUnits(out, 0xD800 + (v >> 10), 2);
				appendCodeUnits(out, 0xDC00 + (v & 0x3FF), 2);
				return;
			}
			for(U32 k = 0; k < unitBytes; ++k)
				out.push_back((C8)(U8)(cp >> (8 * k)));
		}
	} // namespace detail

	// [ 0x | 0X | 0 ] digits [ u | U | l | L ]...
	B32 Parser::parseIntLiteral(const Token& tok, I64& value, U32& bits, U8& mods) {
		const String& s = lex.text(tok);
		B32 isUnsigned = false;
		U32 lCount = 0;

		U32 end = (U32)s.size();
		while(end > 0) {
			C8 c = s[end - 1];
			if(c == 'u' || c == 'U')
				isUnsigned = true;
			else if(c == 'l' || c == 'L')
				++lCount;
			else
				break;
			--end;
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
			if(d < 0 || d >= base)
				return fail(tok, "invalid digit in integer constant");
			v = v * (U64)base + (U64)d;
		}

		value = (I64)v;
		mods = intLiteralMods(v, base == 10, isUnsigned, lCount, bits);
		return true;
	}

	// the first type of C11 6.4.4.1 that holds v
	// the candidates are signed and unsigned pairs, an l or ll suffix starts at a later pair
	U8 Parser::intLiteralMods(U64 v, B32 dec, B32 isUnsigned, U32 lCount, U32& bits) const {
		constexpr U8 kLongLong = CType::Long | CType::LongLong;
		struct Candidate {
			U32 bits;
			U8 mods;
		};
		const Candidate kCandidates[] = {
				{32, 0},
				{32, CType::Unsigned},
				{lay.longBits, CType::Long},
				{lay.longBits, CType::Long | CType::Unsigned},
				{64, kLongLong},
				{64, kLongLong | CType::Unsigned},
		};
		U32 firstPair = std::min(lCount, 2u);
		for(U32 i = firstPair * 2; i < std::size(kCandidates); ++i) {
			const Candidate& c = kCandidates[i];
			B32 isUnsignedType = (c.mods & CType::Unsigned) != 0;
			if(isUnsigned && !isUnsignedType)
				continue; // a u suffix never gives a signed type
			if(dec && !isUnsigned && isUnsignedType)
				continue; // a decimal without u never gives an unsigned type
			U32 valueBits = isUnsignedType ? c.bits : c.bits - 1;
			if(v <= UINT64_MAX >> (64 - valueBits)) {
				bits = c.bits;
				return c.mods;
			}
		}
		bits = 64; // a decimal no signed type holds
		return lCount >= 2 ? kLongLong | CType::Unsigned : kLongLong;
	}

	// after \ : ucn | simple-escape | ? | x hex-digit... | octal-digit [octal-digit [octal-digit]]
	// any other character stands for itself
	B32 Parser::decodeBackslash(
			const String& s, U32& i, U32 end, const Token& tok, U32 maxVal, U32& out) {
		if(detail::ucnAt(s, i, end))
			return decodeUcn(s, i, end, tok, out);
		if(i >= end)
			return fail(tok, "unterminated escape");
		C8 e = s[i++];
		U8 simple = 0;
		if(simpleEscape(e, simple)) {
			out = simple;
			return true;
		}
		if(e == 'x') {
			if(i >= end || !isHexDigit(s[i]))
				return fail(tok, "expected hex digits in escape");
			U32 hv = 0;
			while(i < end && isHexDigit(s[i]))
				hv = hv * 16 + (U32)hexVal(s[i++]);
			if(hv > maxVal)
				return fail(tok, "hex escape out of range");
			out = hv;
			return true;
		}
		if(!isOctalDigit(e)) {
			out = (U8)e;
			return true;
		}
		U32 ov = (U32)(e - '0');
		for(U32 k = 0; k < 2 && i < end && isOctalDigit(s[i]); ++k, ++i)
			ov = ov * 8 + (U32)(s[i] - '0');
		if(ov > maxVal)
			return fail(tok, "octal escape out of range");
		out = ov;
		return true;
	}

	// after \ : u hex-quad | U hex-quad hex-quad
	B32 Parser::decodeUcn(const String& s, U32& i, U32 end, const Token& tok, U32& cp) {
		C8 kind = s[i++]; // 'u' or 'U'
		U32 ndigits = (kind == 'u') ? 4 : 8;
		if(i + ndigits > end)
			return fail(tok, "incomplete universal character name");
		U32 v = 0;
		for(U32 k = 0; k < ndigits; ++k) {
			if(!isHexDigit(s[i + k]))
				return fail(tok, "universal character name requires hex digits");
			v = v * 16 + (U32)hexVal(s[i + k]);
		}
		i += ndigits;
		if((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
			return fail(tok, "invalid universal character name");
		cp = v;
		return true;
	}

	// [ L | u | U ] ' c-char... '
	// several chars pack big-endian into one int
	B32 Parser::parseCharLiteral(const Token& tok, I64& value) {
		const String& s = lex.text(tok);
		U32 maxVal = detail::escapeMaxVal(s.size() ? s[0] : '\'');
		B32 prefixed = s.size() != 0 && s[0] != '\'';
		auto [i, end] = detail::quotedRange(s, '\'');

		if(i >= end)
			return fail(tok, "empty character constant");

		I64 v = 0;
		U32 count = 0;
		while(i < end) {
			I64 c;
			if(s[i] == '\\') {
				++i;
				B32 wide = detail::ucnAt(s, i, end) || maxVal > 0xFFu;
				U32 val;
				if(!decodeBackslash(s, i, end, tok, maxVal, val))
					return false;
				c = wide ? (I64)(I32)val : (I64)(I8)(U8)val;
			} else if(prefixed) {
				c = (I64)detail::utf8Decode(s, i, end); // L/u/U hold one code point
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

	// [ L | u | u8 | U ] " s-char... "
	// appended to out as utf-8 bytes when unitBytes is 1, else as unitBytes-wide code units
	B32 Parser::parseStringLiteral(const Token& tok, U32 unitBytes, String& out) {
		const String& s = lex.text(tok);
		B32 isU8Bytes = unitBytes == 1 && s.size() > 1 && s[0] == 'u' && s[1] == '8';
		U32 maxVal = isU8Bytes ? 0xFFu : detail::escapeMaxVal(s.size() ? s[0] : '"');
		auto [i, end] = detail::quotedRange(s, '"');
		while(i < end) {
			if(s[i] != '\\') {
				if(unitBytes == 1)
					out.push_back(s[i++]);
				else
					detail::appendCodeUnits(out, detail::utf8Decode(s, i, end), unitBytes);
				continue;
			}
			++i;
			B32 wide = detail::ucnAt(s, i, end) || maxVal > 0xFFu;
			U32 val;
			if(!decodeBackslash(s, i, end, tok, maxVal, val))
				return false;
			// a wide escape carries its full value, it is not cut to a byte
			if(unitBytes != 1)
				detail::appendCodeUnits(out, val, unitBytes);
			else if(wide)
				detail::utf8Encode(out, val);
			else
				out.push_back((C8)(U8)val);
		}
		return true;
	}
} // namespace rat::cc
