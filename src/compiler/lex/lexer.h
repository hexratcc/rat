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
		U32 offset = 0; // index of the token in its stream
		U32 line = 1;		// 1-based line of the first character
		U32 col = 1;		// 1-based column of the first character
	};

	namespace detail {
		B32 validIntSuffix(const C8* s, U32 n);
		B32 validFloatSuffix(const C8* s, U32 n);

		// scanners over one token spelling; advance i, false on error with err set
		B32 scanSuffix(const C8* s, U32 n, U32& i, B32 isFloat, TokKind& kind, String& err);
		B32 scanHexNumber(const C8* s, U32 n, U32& i, B32& isFloat, String& err);
		B32 scanDecNumber(const C8* s, U32 n, U32& i, B32& isFloat, String& err);
		U32 encodingPrefix(const C8* s, U32 n);
		B32 scanNumber(const C8* s, U32 n, U32& i, TokKind& kind, String& err);
		B32 scanQuoted(const C8* s, U32 n, U32& i, C8 quote, const C8* unterminated, String& err);

		// kind of one complete pp-number, char constant or string literal
		TokKind classifyLiteral(const String& text, String& err);
	} // namespace detail

	const C8* tokKindName(TokKind kind);
} // namespace rat::cc

#endif
