#ifndef RAT_CC_LEXER_H
#define RAT_CC_LEXER_H

#include "core.h"

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
		KwBool,					// _Bool
		KwComplex,			// _Complex
		KwImaginary,		// _Imaginary
		KwGeneric,			// _Generic
		KwStaticAssert, // _Static_assert
		KwReal,					// __real__ (extract the real part of a complex value)
		KwImag,					// __imag__ (extract the imaginary part of a complex value)
		KwTypeof,				// typeof / __typeof / __typeof__ (GCC: type of an expr)
		KwNoinline, // __rat_noinline__ (marker the preprocessor leaves for __attribute__((noinline)))
		KwAlias,		// __rat_alias__ (marker the preprocessor leaves for __attribute__((alias(...))))
		KwAsm,			// asm / __asm / __asm__ (GNU inline assembly)
		KwAlignof,	// _Alignof (__alignof__ / __alignof are macros for it)
		KwAlignas,	// _Alignas (__attribute__((aligned(n))) expands to it too)

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
		U32 line = 1;		// 1-based line of the first character
		U32 col = 1;		// 1-based column of the first character
	};

	namespace detail {
		B32 validIntSuffix(const C8* s, U32 n);
		B32 validFloatSuffix(const C8* s, U32 n);
		B32 spellingIs(const C8* k, const C8* s, U32 n);
		TokKind keywordKind(const C8* s, U32 n);
	} // namespace detail

	struct Lexer {
		Lexer(const C8* src, U32 len);

		Token next();

		String text(const Token& tok) const;

		const String& error() const { return errMsg; }
	private:
		void skipTrivia();
		void bump();

		Token lexIdentifier(Token tok);
		Token lexNumber(Token tok);
		Token lexIntSuffix(Token tok);
		Token lexFloatSuffix(Token tok);
		Token lexChar(Token tok);
		Token lexString(Token tok);
		Token lexQuoted(Token tok, C8 quote, const C8* unterminated, TokKind kind);
		struct PunctAlt {
			C8 c;
			TokKind kind;
		};
		Token lexPunct(Token tok);
		Token lexAltOp(Token tok, TokKind base, std::initializer_list<PunctAlt> alts);

		Token finish(Token tok, TokKind kind);
		Token fail(Token tok, const String& msg);

		C8 at(U32 i) const { return i < len ? src[i] : '\0'; }
		C8 cur() const { return at(pos); }
		B32 isUcnStart(U32 p) const;

		const C8* src;
		U32 len;
		U32 pos = 0;
		U32 line = 1;
		U32 lineStart = 0;

		String errMsg;
	};

	const C8* tokKindName(TokKind kind);
} // namespace rat::cc

#endif
