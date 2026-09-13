#include "lex/lexer.h"

#include "lex/char_class.h"

namespace rat::cc {
	namespace detail {
		// clang-format off
		const C8* const kTokNames[] = {
				// literals and specials
				"eof", "error", "identifier", "int-constant", "float-constant", "char-constant",
				"string-literal",
				// keywords
				"auto", "break", "case", "char", "const", "continue", "default", "do", "double",
				"else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
				"register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
				"switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
				"_Complex", "_Imaginary", "_Generic", "_Static_assert", "__real__", "__imag__",
				"typeof", "__rat_noinline__", "__rat_alias__", "asm", "_Alignof", "_Alignas",
				// punctuation
				"(", ")", "{", "}", "[", "]", ";", ",", ".", "->", "...", "+", "-", "*", "/", "%",
				"++", "--", "&", "|", "^", "~", "!", "&&", "||", "<", ">", "<=", ">=", "==", "!=",
				"<<", ">>", "?", ":", "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=",
				">>=",
		};
		// clang-format on
		static_assert(sizeof(kTokNames) / sizeof(kTokNames[0]) == (U32)TokKind::ShrEq + 1,
									"kTokNames must cover every TokKind");

		B32 validIntSuffix(const C8* s, U32 n) {
			B32 haveU = false, haveL = false;
			U32 i = 0;
			while(i < n) {
				C8 c = s[i];
				if((c == 'u' || c == 'U') && !haveU) {
					haveU = true;
					++i;
				} else if((c == 'l' || c == 'L') && !haveL) {
					haveL = true;
					// a long-long suffix must repeat the same letter
					if(i + 1 < n && s[i + 1] == c)
						i += 2;
					else
						++i;
				} else {
					return false;
				}
			}
			return true;
		}

		B32 validFloatSuffix(const C8* s, U32 n) {
			B32 haveSize = false, imag = false;
			for(U32 i = 0; i < n; ++i) {
				C8 c = s[i];
				if(c == 'f' || c == 'F' || c == 'l' || c == 'L') {
					if(haveSize)
						return false;
					haveSize = true;
				} else if(c == 'i' || c == 'I' || c == 'j' || c == 'J') {
					if(imag)
						return false;
					imag = true;
				} else
					return false;
			}
			return true;
		}

		// character at i, '\0' past the end
		inline C8 charAt(const C8* s, U32 n, U32 i) { return i < n ? s[i] : '\0'; }

		B32 scanSuffix(const C8* s, U32 n, U32& i, B32 isFloat, TokKind& kind, String& err) {
			U32 start = i;
			while(isIdentCont(charAt(s, n, i)))
				++i;
			if(isFloat) {
				if(!validFloatSuffix(s + start, i - start)) {
					err = "invalid suffix on floating constant";
					return false;
				}
				kind = TokKind::FloatConstant;
				return true;
			}
			if(!validIntSuffix(s + start, i - start)) {
				err = "invalid suffix on integer constant";
				return false;
			}
			kind = TokKind::IntConstant;
			return true;
		}

		B32 scanHexNumber(const C8* s, U32 n, U32& i, B32& isFloat, String& err) {
			i += 2; // "0x"
			B32 anyDigits = false;
			while(isHexDigit(charAt(s, n, i))) {
				anyDigits = true;
				++i;
			}
			if(charAt(s, n, i) == '.') {
				isFloat = true;
				++i;
				while(isHexDigit(charAt(s, n, i))) {
					anyDigits = true;
					++i;
				}
			}
			if(!anyDigits) {
				err = "expected hex digits after '0x'";
				return false;
			}
			C8 c = charAt(s, n, i);
			if(c != 'p' && c != 'P') {
				if(!isFloat)
					return true;
				err = "hexadecimal floating constant requires an exponent";
				return false;
			}
			isFloat = true;
			++i;
			if(charAt(s, n, i) == '+' || charAt(s, n, i) == '-')
				++i;
			if(!isDigit(charAt(s, n, i))) {
				err = "expected digits in binary exponent";
				return false;
			}
			while(isDigit(charAt(s, n, i)))
				++i;
			return true;
		}

		B32 scanDecNumber(const C8* s, U32 n, U32& i, B32& isFloat, String& err) {
			while(isDigit(charAt(s, n, i)))
				++i;
			if(charAt(s, n, i) == '.') {
				isFloat = true;
				++i;
				while(isDigit(charAt(s, n, i)))
					++i;
			}
			C8 c = charAt(s, n, i);
			if(c != 'e' && c != 'E')
				return true;
			isFloat = true;
			++i;
			if(charAt(s, n, i) == '+' || charAt(s, n, i) == '-')
				++i;
			if(!isDigit(charAt(s, n, i))) {
				err = "expected digits in exponent";
				return false;
			}
			while(isDigit(charAt(s, n, i)))
				++i;
			return true;
		}

		B32 scanNumber(const C8* s, U32 n, U32& i, TokKind& kind, String& err) {
			B32 isFloat = false;
			C8 c1 = charAt(s, n, i + 1);
			B32 hex = charAt(s, n, i) == '0' && (c1 == 'x' || c1 == 'X');
			B32 ok = hex ? scanHexNumber(s, n, i, isFloat, err) : scanDecNumber(s, n, i, isFloat, err);
			if(!ok)
				return false;
			return scanSuffix(s, n, i, isFloat, kind, err);
		}

		B32 scanQuoted(const C8* s, U32 n, U32& i, C8 quote, const C8* unterminated, String& err) {
			++i; // opening quote
			while(i < n && s[i] != quote && s[i] != '\n') {
				if(s[i] == '\\')
					++i; // an escaped character never closes the literal
				++i;
			}
			if(charAt(s, n, i) != quote) {
				err = unterminated;
				return false;
			}
			++i; // closing quote
			return true;
		}

		// length of an encoding prefix: L/u/U before a quote, u8 before '"'
		U32 encodingPrefix(const C8* s, U32 n) {
			C8 c = charAt(s, n, 0), c1 = charAt(s, n, 1);
			if(c != 'L' && c != 'u' && c != 'U')
				return 0;
			if(c == 'u' && c1 == '8' && charAt(s, n, 2) == '"')
				return 2;
			return (c1 == '\'' || c1 == '"') ? 1 : 0;
		}

		TokKind classifyLiteral(const String& text, String& err) {
			const C8* s = text.data();
			U32 n = (U32)text.size();
			U32 i = encodingPrefix(s, n);
			TokKind kind = TokKind::Error;
			B32 ok = false;
			C8 c = charAt(s, n, i);
			if(c == '\'') {
				ok = scanQuoted(s, n, i, '\'', "unterminated character constant", err);
				kind = TokKind::CharConstant;
			} else if(c == '"') {
				ok = scanQuoted(s, n, i, '"', "unterminated string literal", err);
				kind = TokKind::StringLiteral;
			} else {
				i = 0; // no prefix on a pp-number
				ok = scanNumber(s, n, i, kind, err);
			}
			if(!ok)
				return TokKind::Error;
			if(i != n) {
				err = "malformed token '" + text + "'";
				return TokKind::Error;
			}
			return kind;
		}
	} // namespace detail

	const C8* tokKindName(TokKind kind) { return detail::kTokNames[(U32)kind]; }
} // namespace rat::cc
