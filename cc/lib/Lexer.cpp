#include "Lexer.h"

#include "CharClass.h"

namespace rat::cc {
	namespace {
		struct Keyword {
			const char* text;
			TokKind kind;
		};

		const Keyword kKeywords[] = {
				{"auto", TokKind::KwAuto},
				{"break", TokKind::KwBreak},
				{"case", TokKind::KwCase},
				{"char", TokKind::KwChar},
				{"const", TokKind::KwConst},
				{"continue", TokKind::KwContinue},
				{"default", TokKind::KwDefault},
				{"do", TokKind::KwDo},
				{"double", TokKind::KwDouble},
				{"else", TokKind::KwElse},
				{"enum", TokKind::KwEnum},
				{"extern", TokKind::KwExtern},
				{"float", TokKind::KwFloat},
				{"for", TokKind::KwFor},
				{"goto", TokKind::KwGoto},
				{"if", TokKind::KwIf},
				{"inline", TokKind::KwInline},
				{"int", TokKind::KwInt},
				{"long", TokKind::KwLong},
				{"register", TokKind::KwRegister},
				{"restrict", TokKind::KwRestrict},
				{"return", TokKind::KwReturn},
				{"short", TokKind::KwShort},
				{"signed", TokKind::KwSigned},
				{"sizeof", TokKind::KwSizeof},
				{"static", TokKind::KwStatic},
				{"struct", TokKind::KwStruct},
				{"switch", TokKind::KwSwitch},
				{"typedef", TokKind::KwTypedef},
				{"union", TokKind::KwUnion},
				{"unsigned", TokKind::KwUnsigned},
				{"void", TokKind::KwVoid},
				{"volatile", TokKind::KwVolatile},
				{"while", TokKind::KwWhile},
				{"_Bool", TokKind::KwBool},
				{"_Complex", TokKind::KwComplex},
				{"_Imaginary", TokKind::KwImaginary},
				{"_Generic", TokKind::KwGeneric},
				{"_Static_assert", TokKind::KwStaticAssert},
				{"__real__", TokKind::KwReal},
				{"__imag__", TokKind::KwImag},
				{"typeof", TokKind::KwTypeof},
				{"__typeof", TokKind::KwTypeof},
				{"__typeof__", TokKind::KwTypeof},
		};

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

		TokKind keywordKind(const char* s, U32 n) {
			for(const Keyword& kw : kKeywords) {
				const char* k = kw.text;
				U32 i = 0;
				for(; i < n && k[i] && k[i] == s[i]; ++i)
					;
				if(i == n && k[i] == '\0')
					return kw.kind;
			}
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

		switch(c) {
		case '(':
			bump();
			return finish(tok, TokKind::LParen);
		case ')':
			bump();
			return finish(tok, TokKind::RParen);
		case '{':
			bump();
			return finish(tok, TokKind::LBrace);
		case '}':
			bump();
			return finish(tok, TokKind::RBrace);
		case '[':
			bump();
			return finish(tok, TokKind::LBracket);
		case ']':
			bump();
			return finish(tok, TokKind::RBracket);
		case ';':
			bump();
			return finish(tok, TokKind::Semicolon);
		case ',':
			bump();
			return finish(tok, TokKind::Comma);
		case '~':
			bump();
			return finish(tok, TokKind::Tilde);
		case '?':
			bump();
			return finish(tok, TokKind::Question);
		case ':':
			bump();
			return finish(tok, TokKind::Colon);
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

	const char* tokKindName(TokKind kind) {
		switch(kind) {
		case TokKind::Eof:
			return "eof";
		case TokKind::Error:
			return "error";
		case TokKind::Identifier:
			return "identifier";
		case TokKind::IntConstant:
			return "int-constant";
		case TokKind::FloatConstant:
			return "float-constant";
		case TokKind::CharConstant:
			return "char-constant";
		case TokKind::StringLiteral:
			return "string-literal";
		case TokKind::KwAuto:
			return "auto";
		case TokKind::KwBreak:
			return "break";
		case TokKind::KwCase:
			return "case";
		case TokKind::KwChar:
			return "char";
		case TokKind::KwConst:
			return "const";
		case TokKind::KwContinue:
			return "continue";
		case TokKind::KwDefault:
			return "default";
		case TokKind::KwDo:
			return "do";
		case TokKind::KwDouble:
			return "double";
		case TokKind::KwElse:
			return "else";
		case TokKind::KwEnum:
			return "enum";
		case TokKind::KwExtern:
			return "extern";
		case TokKind::KwFloat:
			return "float";
		case TokKind::KwFor:
			return "for";
		case TokKind::KwGoto:
			return "goto";
		case TokKind::KwIf:
			return "if";
		case TokKind::KwInline:
			return "inline";
		case TokKind::KwInt:
			return "int";
		case TokKind::KwLong:
			return "long";
		case TokKind::KwRegister:
			return "register";
		case TokKind::KwRestrict:
			return "restrict";
		case TokKind::KwReturn:
			return "return";
		case TokKind::KwShort:
			return "short";
		case TokKind::KwSigned:
			return "signed";
		case TokKind::KwSizeof:
			return "sizeof";
		case TokKind::KwStatic:
			return "static";
		case TokKind::KwStruct:
			return "struct";
		case TokKind::KwSwitch:
			return "switch";
		case TokKind::KwTypedef:
			return "typedef";
		case TokKind::KwUnion:
			return "union";
		case TokKind::KwUnsigned:
			return "unsigned";
		case TokKind::KwVoid:
			return "void";
		case TokKind::KwVolatile:
			return "volatile";
		case TokKind::KwWhile:
			return "while";
		case TokKind::KwBool:
			return "_Bool";
		case TokKind::KwComplex:
			return "_Complex";
		case TokKind::KwImaginary:
			return "_Imaginary";
		case TokKind::KwGeneric:
			return "_Generic";
		case TokKind::KwStaticAssert:
			return "_Static_assert";
		case TokKind::KwReal:
			return "__real__";
		case TokKind::KwImag:
			return "__imag__";
		case TokKind::KwTypeof:
			return "typeof";
		case TokKind::LParen:
			return "(";
		case TokKind::RParen:
			return ")";
		case TokKind::LBrace:
			return "{";
		case TokKind::RBrace:
			return "}";
		case TokKind::LBracket:
			return "[";
		case TokKind::RBracket:
			return "]";
		case TokKind::Semicolon:
			return ";";
		case TokKind::Comma:
			return ",";
		case TokKind::Dot:
			return ".";
		case TokKind::Arrow:
			return "->";
		case TokKind::Ellipsis:
			return "...";
		case TokKind::Plus:
			return "+";
		case TokKind::Minus:
			return "-";
		case TokKind::Star:
			return "*";
		case TokKind::Slash:
			return "/";
		case TokKind::Percent:
			return "%";
		case TokKind::PlusPlus:
			return "++";
		case TokKind::MinusMinus:
			return "--";
		case TokKind::Amp:
			return "&";
		case TokKind::Pipe:
			return "|";
		case TokKind::Caret:
			return "^";
		case TokKind::Tilde:
			return "~";
		case TokKind::Bang:
			return "!";
		case TokKind::AmpAmp:
			return "&&";
		case TokKind::PipePipe:
			return "||";
		case TokKind::Lt:
			return "<";
		case TokKind::Gt:
			return ">";
		case TokKind::Le:
			return "<=";
		case TokKind::Ge:
			return ">=";
		case TokKind::EqEq:
			return "==";
		case TokKind::BangEq:
			return "!=";
		case TokKind::Shl:
			return "<<";
		case TokKind::Shr:
			return ">>";
		case TokKind::Question:
			return "?";
		case TokKind::Colon:
			return ":";
		case TokKind::Assign:
			return "=";
		case TokKind::PlusEq:
			return "+=";
		case TokKind::MinusEq:
			return "-=";
		case TokKind::StarEq:
			return "*=";
		case TokKind::SlashEq:
			return "/=";
		case TokKind::PercentEq:
			return "%=";
		case TokKind::AmpEq:
			return "&=";
		case TokKind::PipeEq:
			return "|=";
		case TokKind::CaretEq:
			return "^=";
		case TokKind::ShlEq:
			return "<<=";
		case TokKind::ShrEq:
			return ">>=";
		}
		return "?";
	}
} // namespace rat::cc
