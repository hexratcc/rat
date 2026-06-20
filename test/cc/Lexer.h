#ifndef RAT_CC_LEXER_H
#define RAT_CC_LEXER_H

#include "Core.h"

namespace rat::cc {
	enum class TokKind : U8 {
		Eof,
		Error,

		// literals and identifiers
		Identifier,
		IntConstant,
		FloatConstant,
		CharConstant,
		StringLiteral,

		// keywords (C99)
		KwAuto,
		KwBreak,
		KwCase,
		KwChar,
		KwConst,
		KwContinue,
		KwDefault,
		KwDo,
		KwDouble,
		KwElse,
		KwEnum,
		KwExtern,
		KwFloat,
		KwFor,
		KwGoto,
		KwIf,
		KwInline,
		KwInt,
		KwLong,
		KwRegister,
		KwRestrict,
		KwReturn,
		KwShort,
		KwSigned,
		KwSizeof,
		KwStatic,
		KwStruct,
		KwSwitch,
		KwTypedef,
		KwUnion,
		KwUnsigned,
		KwVoid,
		KwVolatile,
		KwWhile,
		KwBool,      // _Bool
		KwComplex,   // _Complex
		KwImaginary, // _Imaginary

		// punctuators
		LParen,
		RParen,
		LBrace,
		RBrace,
		LBracket,
		RBracket,
		Semicolon,
		Comma,
		Dot,
		Arrow,
		Ellipsis,
		Plus,
		Minus,
		Star,
		Slash,
		Percent,
		PlusPlus,
		MinusMinus,
		Amp,
		Pipe,
		Caret,
		Tilde,
		Bang,
		AmpAmp,
		PipePipe,
		Lt,
		Gt,
		Le,
		Ge,
		EqEq,
		BangEq,
		Shl,
		Shr,
		Question,
		Colon,
		Assign,
		PlusEq,
		MinusEq,
		StarEq,
		SlashEq,
		PercentEq,
		AmpEq,
		PipeEq,
		CaretEq,
		ShlEq,
		ShrEq,
	};

	struct Token {
		TokKind kind = TokKind::Eof;
		U32 offset = 0; // byte offset of the lexeme in the source buffer
		U32 length = 0; // lexeme length in bytes
		U32 line = 1;   // 1-based line of the first character
		U32 col = 1;    // 1-based column of the first character
	};

	struct Lexer {
		Lexer(const char* src, U32 len, String file = "<input>");

		Token next();
		const Token& peek();

		String text(const Token& tok) const;

		const String& file() const { return fileName; }
		const char* source() const { return src; }
		U32 sourceLength() const { return len; }
		const String& error() const { return errMsg; }

	private:
		Token scan();
		void skipTrivia();
		void bump();

		Token lexIdentifier(Token tok);
		Token lexNumber(Token tok);
		Token lexChar(Token tok);
		Token lexString(Token tok);
		Token lexPunct(Token tok);

		Token finish(Token tok, TokKind kind);
		Token fail(Token tok, const String& msg);

		char at(U32 i) const { return i < len ? src[i] : '\0'; }
		char cur() const { return at(pos); }

		const char* src;
		U32 len;
		U32 pos = 0;
		U32 line = 1;
		U32 lineStart = 0;

		String fileName;
		String errMsg;

		Token lookahead;
		B32 hasLookahead = false;
	};

	const char* tokKindName(TokKind kind);
} // namespace rat::cc

#endif
