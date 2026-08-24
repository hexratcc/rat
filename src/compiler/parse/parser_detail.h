#ifndef RAT_CC_PARSER_DETAIL_H
#define RAT_CC_PARSER_DETAIL_H

#include "lex/lexer.h"
#include "parse/ast.h"

namespace rat::cc::detail {
	constexpr I64 kIntMin = -2147483648LL;
	constexpr I64 kIntMax = 2147483647LL;

	struct BinInfo {
		I32 prec;
		ExprOp op;
	};

	BinInfo binInfo(TokKind kind);
	B32 assignOp(TokKind kind, ExprOp& op);
	B32 unaryOp(TokKind kind, ExprOp& op);
	void utf8Encode(String& out, U32 cp);
	U32 utf8Decode(const String& bytes, U32& i, U32 n);
	void appendCodeUnits(String& out, U32 cp, U32 unitBytes);

	B32 isTypeQualifier(TokKind kind);
	B32 isQualOrStorage(TokKind kind);
	B32 isTypeStart(TokKind kind);

	U64 alignUp(U64 value, U32 align);
} // namespace rat::cc::detail

#endif
