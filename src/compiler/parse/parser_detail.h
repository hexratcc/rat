#ifndef RAT_CC_PARSER_DETAIL_H
#define RAT_CC_PARSER_DETAIL_H

#include "lex/lexer.h"
#include "parse/ast.h"

namespace rat::cc::detail {
	constexpr ExprOp kNoOp = (ExprOp)0xFF; // no operator

	// what one token kind means to the parser, see kTokSyntax
	struct TokSyntax {
		enum Spec : U8 {
			Qualifier = 1, // parsePointers takes these too
			Storage = 2,	 // counts toward the one storage class allowed
			FuncSpec = 4,
			TypeWord = 8,
			TypeHead = 16, // enum, struct, union, typeof, alignas
		};
		I32 binaryPrec = 0; // 0 when not a binary operator
		ExprOp binaryOp = kNoOp;
		ExprOp unaryOp = kNoOp;
		ExprOp assignOp = kNoOp;
		U8 spec = 0;
	};

	struct TokSyntaxTable {
		struct BinaryRow {
			TokKind kind;
			I32 prec;
			ExprOp op;
		};
		struct OpRow {
			TokKind kind;
			ExprOp op;
		};
		struct SpecRow {
			TokKind kind;
			U8 spec;
		};

		TokSyntax rows[(U32)TokKind::ShrEq + 1] = {};

		constexpr TokSyntaxTable() {
			// clang-format off
			constexpr BinaryRow kBinary[] = {
					{TokKind::Plus, 9, ExprOp::Add},       {TokKind::Minus, 9, ExprOp::Sub},
					{TokKind::Star, 10, ExprOp::Mul},      {TokKind::Slash, 10, ExprOp::Div},
					{TokKind::Percent, 10, ExprOp::Rem},   {TokKind::Amp, 5, ExprOp::BitAnd},
					{TokKind::Pipe, 3, ExprOp::BitOr},     {TokKind::Caret, 4, ExprOp::BitXor},
					{TokKind::AmpAmp, 2, ExprOp::LogAnd},  {TokKind::PipePipe, 1, ExprOp::LogOr},
					{TokKind::Lt, 7, ExprOp::Lt},          {TokKind::Gt, 7, ExprOp::Gt},
					{TokKind::Le, 7, ExprOp::Le},          {TokKind::Ge, 7, ExprOp::Ge},
					{TokKind::EqEq, 6, ExprOp::Eq},        {TokKind::BangEq, 6, ExprOp::Ne},
					{TokKind::Shl, 8, ExprOp::Shl},        {TokKind::Shr, 8, ExprOp::Shr},
			};
			constexpr OpRow kUnary[] = {
					{TokKind::Plus, ExprOp::Pos},          {TokKind::Minus, ExprOp::Neg},
					{TokKind::Bang, ExprOp::Not},          {TokKind::Tilde, ExprOp::BitNot},
					{TokKind::Amp, ExprOp::Addr},          {TokKind::Star, ExprOp::Deref},
					{TokKind::KwReal, ExprOp::Real},       {TokKind::KwImag, ExprOp::Imag},
					{TokKind::PlusPlus, ExprOp::PreInc},   {TokKind::MinusMinus, ExprOp::PreDec},
			};
			constexpr OpRow kAssign[] = {
					{TokKind::Assign, ExprOp::Assign},     {TokKind::PlusEq, ExprOp::AddAssign},
					{TokKind::MinusEq, ExprOp::SubAssign}, {TokKind::StarEq, ExprOp::MulAssign},
					{TokKind::SlashEq, ExprOp::DivAssign}, {TokKind::PercentEq, ExprOp::RemAssign},
					{TokKind::AmpEq, ExprOp::AndAssign},   {TokKind::PipeEq, ExprOp::OrAssign},
					{TokKind::CaretEq, ExprOp::XorAssign}, {TokKind::ShlEq, ExprOp::ShlAssign},
					{TokKind::ShrEq, ExprOp::ShrAssign},
			};
			constexpr SpecRow kSpec[] = {
					{TokKind::KwConst, TokSyntax::Qualifier},
					{TokKind::KwVolatile, TokSyntax::Qualifier},
					{TokKind::KwRestrict, TokSyntax::Qualifier},
					{TokKind::KwNoinline, TokSyntax::Qualifier},
					{TokKind::KwStatic, TokSyntax::Storage},
					{TokKind::KwExtern, TokSyntax::Storage},
					{TokKind::KwAuto, TokSyntax::Storage},
					{TokKind::KwRegister, TokSyntax::Storage},
					{TokKind::KwInline, TokSyntax::FuncSpec},
			};
			constexpr TokKind kTypeWords[] = {
					TokKind::KwVoid, TokKind::KwBool, TokKind::KwChar, TokKind::KwShort, TokKind::KwInt,
					TokKind::KwLong, TokKind::KwFloat, TokKind::KwDouble, TokKind::KwSigned,
					TokKind::KwUnsigned, TokKind::KwComplex, TokKind::KwImaginary,
			};
			constexpr TokKind kTypeHeads[] = {
					TokKind::KwEnum, TokKind::KwStruct, TokKind::KwUnion, TokKind::KwTypeof, TokKind::KwAlignas,
			};
			// clang-format on
			for(const BinaryRow& row : kBinary) {
				rows[(U32)row.kind].binaryPrec = row.prec;
				rows[(U32)row.kind].binaryOp = row.op;
			}
			for(const OpRow& row : kUnary)
				rows[(U32)row.kind].unaryOp = row.op;
			for(const OpRow& row : kAssign)
				rows[(U32)row.kind].assignOp = row.op;
			for(const SpecRow& row : kSpec)
				rows[(U32)row.kind].spec = row.spec;
			for(TokKind kind : kTypeWords)
				rows[(U32)kind].spec = TokSyntax::TypeWord;
			for(TokKind kind : kTypeHeads)
				rows[(U32)kind].spec = TokSyntax::TypeHead;
		}
	};

	inline constexpr TokSyntaxTable kTokSyntax;

	inline const TokSyntax& tokSyntax(TokKind kind) { return kTokSyntax.rows[(U32)kind]; }
	B32 isTypeQualifier(TokKind kind);
	B32 isQualOrStorage(TokKind kind);

	void utf8Encode(String& out, U32 cp);
	U32 utf8Decode(const String& bytes, U32& i, U32 n);
	void appendCodeUnits(String& out, U32 cp, U32 unitBytes);
	U32 escapeMaxVal(C8 prefix);
	B32 ucnAt(const String& s, U32 i, U32 end);
	B32 isAbsName(const String& name);
	Pair<U32, U32> quotedRange(const String& s, C8 quote);
} // namespace rat::cc::detail

#endif
