#include "Lex/Lexer.h"

#include "Lex/CharClass.h"

namespace rat::cc {
	namespace {
		// clang-format off
		const char* const kTokNames[] = {
				// literals and specials
				"eof", "error", "identifier", "int-constant", "float-constant", "char-constant",
				"string-literal",
				// keywords
				"auto", "break", "case", "char", "const", "continue", "default", "do", "double",
				"else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
				"register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
				"switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
				"_Complex", "_Imaginary", "_Generic", "_Static_assert", "__real__", "__imag__",
				"typeof",
				// punctuation
				"(", ")", "{", "}", "[", "]", ";", ",", ".", "->", "...", "+", "-", "*", "/", "%",
				"++", "--", "&", "|", "^", "~", "!", "&&", "||", "<", ">", "<=", ">=", "==", "!=",
				"<<", ">>", "?", ":", "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=",
				">>=",
		};
		// clang-format on
		static_assert(sizeof(kTokNames) / sizeof(kTokNames[0]) == (U32)TokKind::ShrEq + 1,
									"kTokNames must cover every TokKind");

		B32 validIntSuffix(const char* s, U32 n) {
			B32 haveU = false, haveL = false;
			U32 i = 0;
			while(i < n) {
				char c = s[i];
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

		B32 validFloatSuffix(const char* s, U32 n) {
			B32 haveSize = false, imag = false;
			for(U32 i = 0; i < n; ++i) {
				char c = s[i];
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

		B32 spellingIs(const char* k, const char* s, U32 n) {
			U32 i = 0;
			for(; i < n && k[i] && k[i] == s[i]; ++i)
				;
			return i == n && k[i] == '\0';
		}

		TokKind keywordKind(const char* s, U32 n) {
			for(U32 k = (U32)TokKind::KwAuto; k <= (U32)TokKind::KwTypeof; ++k)
				if(spellingIs(kTokNames[k], s, n))
					return (TokKind)k;
			if(spellingIs("__typeof", s, n) || spellingIs("__typeof__", s, n))
				return TokKind::KwTypeof;
			return TokKind::Identifier;
		}
	} // namespace

	Lexer::Lexer(const char* src, U32 len, String fileName)
	: src(src),
		len(len),
		fileName(std::move(fileName)) {}

	void Lexer::bump() {
		if(pos < len && src[pos] == '\n') {
			++line;
			lineStart = pos + 1;
		}
		++pos;
	}

	void Lexer::skipTrivia() {
		for(;;) {
			char c = cur();
			if(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
				bump();
			} else if(c == '/' && at(pos + 1) == '/') {
				bump();
				bump();
				while(pos < len && cur() != '\n')
					bump();
			} else if(c == '/' && at(pos + 1) == '*') {
				bump();
				bump();
				while(pos < len && !(cur() == '*' && at(pos + 1) == '/'))
					bump();
				if(pos < len) {
					bump(); // '*'
					bump(); // '/'
				}
			} else {
				return;
			}
		}
	}

	Token Lexer::finish(Token tok, TokKind kind) {
		tok.kind = kind;
		tok.length = pos - tok.offset;
		return tok;
	}

	Token Lexer::fail(Token tok, const String& msg) {
		errMsg = msg;
		if(pos == tok.offset && pos < len)
			bump(); // always make progress
		tok.kind = TokKind::Error;
		tok.length = pos - tok.offset;
		return tok;
	}

	Token Lexer::scan() {
		skipTrivia();

		Token tok;
		tok.offset = pos;
		tok.line = line;
		tok.col = pos - lineStart + 1;

		if(pos >= len)
			return finish(tok, TokKind::Eof);

		char c = cur();
		if(c == 'L' || c == 'u' || c == 'U') {
			char n1 = at(pos + 1);
			if(c == 'u' && n1 == '8' && at(pos + 2) == '"') {
				bump(); // 'u'
				bump(); // '8'
				return lexString(tok);
			}
			if(n1 == '\'') {
				bump(); // prefix
				return lexChar(tok);
			}
			if(n1 == '"') {
				bump(); // prefix
				return lexString(tok);
			}
		}
		if(isIdentStart(c) || isUcnStart(pos))
			return lexIdentifier(tok);
		if(isDigit(c) || (c == '.' && isDigit(at(pos + 1))))
			return lexNumber(tok);
		if(c == '\'')
			return lexChar(tok);
		if(c == '"')
			return lexString(tok);
		return lexPunct(tok);
	}

	B32 Lexer::isUcnStart(U32 p) const {
		if(at(p) != '\\')
			return false;
		char n = at(p + 1);
		return n == 'u' || n == 'U';
	}

	Token Lexer::lexIdentifier(Token tok) {
		for(;;) {
			if(isIdentCont(cur())) {
				bump();
			} else if(isUcnStart(pos)) {
				bump(); // backslash
				char kind = cur();
				bump(); // 'u' or 'U'
				U32 ndigits = (kind == 'u') ? 4 : 8;
				for(U32 k = 0; k < ndigits; ++k) {
					if(!isHexDigit(cur()))
						return fail(tok, "incomplete universal character name");
					bump();
				}
			} else {
				break;
			}
		}
		U32 n = pos - tok.offset;
		return finish(tok, keywordKind(src + tok.offset, n));
	}

	Token Lexer::lexNumber(Token tok) {
		if(cur() == '0' && (at(pos + 1) == 'x' || at(pos + 1) == 'X')) {
			bump();
			bump();
			B32 anyDigits = false;
			while(isHexDigit(cur())) {
				anyDigits = true;
				bump();
			}
			B32 isFloat = false;
			if(cur() == '.') {
				isFloat = true;
				bump();
				while(isHexDigit(cur())) {
					anyDigits = true;
					bump();
				}
			}
			if(!anyDigits)
				return fail(tok, "expected hex digits after '0x'");
			if(cur() == 'p' || cur() == 'P') {
				isFloat = true;
				bump();
				if(cur() == '+' || cur() == '-')
					bump();
				if(!isDigit(cur()))
					return fail(tok, "expected digits in binary exponent");
				while(isDigit(cur()))
					bump();
			} else if(isFloat) {
				return fail(tok, "hexadecimal floating constant requires an exponent");
			}
			if(isFloat)
				return lexFloatSuffix(tok);
			return lexIntSuffix(tok);
		}

		B32 isFloat = false;
		while(isDigit(cur()))
			bump();
		if(cur() == '.') {
			isFloat = true;
			bump();
			while(isDigit(cur()))
				bump();
		}
		if(cur() == 'e' || cur() == 'E') {
			isFloat = true;
			bump();
			if(cur() == '+' || cur() == '-')
				bump();
			if(!isDigit(cur()))
				return fail(tok, "expected digits in exponent");
			while(isDigit(cur()))
				bump();
		}

		if(isFloat)
			return lexFloatSuffix(tok);
		return lexIntSuffix(tok);
	}

	Token Lexer::lexIntSuffix(Token tok) {
		U32 sfxStart = pos;
		while(isIdentCont(cur()))
			bump();
		if(!validIntSuffix(src + sfxStart, pos - sfxStart))
			return fail(tok, "invalid suffix on integer constant");
		return finish(tok, TokKind::IntConstant);
	}

	Token Lexer::lexFloatSuffix(Token tok) {
		U32 sfxStart = pos;
		while(isIdentCont(cur()))
			bump();
		if(!validFloatSuffix(src + sfxStart, pos - sfxStart))
			return fail(tok, "invalid suffix on floating constant");
		return finish(tok, TokKind::FloatConstant);
	}

	Token Lexer::lexQuoted(Token tok, char quote, const char* unterminated, TokKind kind) {
		bump(); // opening quote
		while(pos < len && cur() != quote && cur() != '\n') {
			if(cur() == '\\')
				bump(); // consume the backslash
			bump();
		}
		if(cur() != quote)
			return fail(tok, unterminated);
		bump(); // closing quote
		return finish(tok, kind);
	}

	Token Lexer::lexChar(Token tok) {
		return lexQuoted(tok, '\'', "unterminated character constant", TokKind::CharConstant);
	}

	Token Lexer::lexString(Token tok) {
		return lexQuoted(tok, '"', "unterminated string literal", TokKind::StringLiteral);
	}

	Token Lexer::lexEqSuffixOp(Token tok, TokKind base, TokKind eq) {
		bump();
		if(cur() == '=') {
			bump();
			return finish(tok, eq);
		}
		return finish(tok, base);
	}

	Token Lexer::lexPunct(Token tok) {
		char c = cur();

		struct Simple {
			char c;
			TokKind kind;
		};
		static const Simple kSimple[] = {
				{'(', TokKind::LParen},
				{')', TokKind::RParen},
				{'{', TokKind::LBrace},
				{'}', TokKind::RBrace},
				{'[', TokKind::LBracket},
				{']', TokKind::RBracket},
				{';', TokKind::Semicolon},
				{',', TokKind::Comma},
				{'~', TokKind::Tilde},
				{'?', TokKind::Question},
				{':', TokKind::Colon},
		};
		for(const Simple& s : kSimple)
			if(s.c == c) {
				bump();
				return finish(tok, s.kind);
			}

		switch(c) {
		case '.':
			if(at(pos + 1) == '.' && at(pos + 2) == '.') {
				bump();
				bump();
				bump();
				return finish(tok, TokKind::Ellipsis);
			}
			bump();
			return finish(tok, TokKind::Dot);
		case '+':
			bump();
			if(cur() == '+') {
				bump();
				return finish(tok, TokKind::PlusPlus);
			}
			if(cur() == '=') {
				bump();
				return finish(tok, TokKind::PlusEq);
			}
			return finish(tok, TokKind::Plus);
		case '-':
			bump();
			if(cur() == '-') {
				bump();
				return finish(tok, TokKind::MinusMinus);
			}
			if(cur() == '=') {
				bump();
				return finish(tok, TokKind::MinusEq);
			}
			if(cur() == '>') {
				bump();
				return finish(tok, TokKind::Arrow);
			}
			return finish(tok, TokKind::Minus);
		case '*':
			return lexEqSuffixOp(tok, TokKind::Star, TokKind::StarEq);
		case '/':
			return lexEqSuffixOp(tok, TokKind::Slash, TokKind::SlashEq);
		case '%':
			return lexEqSuffixOp(tok, TokKind::Percent, TokKind::PercentEq);
		case '^':
			return lexEqSuffixOp(tok, TokKind::Caret, TokKind::CaretEq);
		case '!':
			return lexEqSuffixOp(tok, TokKind::Bang, TokKind::BangEq);
		case '=':
			return lexEqSuffixOp(tok, TokKind::Assign, TokKind::EqEq);
		case '&':
			bump();
			if(cur() == '&') {
				bump();
				return finish(tok, TokKind::AmpAmp);
			}
			if(cur() == '=') {
				bump();
				return finish(tok, TokKind::AmpEq);
			}
			return finish(tok, TokKind::Amp);
		case '|':
			bump();
			if(cur() == '|') {
				bump();
				return finish(tok, TokKind::PipePipe);
			}
			if(cur() == '=') {
				bump();
				return finish(tok, TokKind::PipeEq);
			}
			return finish(tok, TokKind::Pipe);
		case '<':
			bump();
			if(cur() == '<') {
				bump();
				if(cur() == '=') {
					bump();
					return finish(tok, TokKind::ShlEq);
				}
				return finish(tok, TokKind::Shl);
			}
			if(cur() == '=') {
				bump();
				return finish(tok, TokKind::Le);
			}
			return finish(tok, TokKind::Lt);
		case '>':
			bump();
			if(cur() == '>') {
				bump();
				if(cur() == '=') {
					bump();
					return finish(tok, TokKind::ShrEq);
				}
				return finish(tok, TokKind::Shr);
			}
			if(cur() == '=') {
				bump();
				return finish(tok, TokKind::Ge);
			}
			return finish(tok, TokKind::Gt);
		default:
			return fail(tok, String("unexpected character '") + c + "'");
		}
	}

	Token Lexer::next() {
		if(hasLookahead) {
			Token t = lookahead;
			if(hasLookahead2) {
				lookahead = lookahead2;
				hasLookahead2 = false;
			} else {
				hasLookahead = false;
			}
			return t;
		}
		return scan();
	}

	const Token& Lexer::peek() {
		if(!hasLookahead) {
			lookahead = scan();
			hasLookahead = true;
		}
		return lookahead;
	}

	const Token& Lexer::peek2() {
		peek();
		if(!hasLookahead2) {
			lookahead2 = scan();
			hasLookahead2 = true;
		}
		return lookahead2;
	}

	String Lexer::text(const Token& tok) const { return String(src + tok.offset, tok.length); }

	const char* tokKindName(TokKind kind) { return kTokNames[(U32)kind]; }
} // namespace rat::cc
