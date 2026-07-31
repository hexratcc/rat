#ifndef RAT_CC_PARSER_DETAIL_H
#define RAT_CC_PARSER_DETAIL_H

#include "Lex/Lexer.h"
#include "Parse/Ast.h"

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
	String decodeUtf8ToUtf32LE(const String& bytes);
	String decodeUtf8ToUtf16LE(const String& bytes);

	B32 isTypeQualifier(TokKind kind);
	B32 isQualOrStorage(TokKind kind);
	B32 isTypeStart(TokKind kind);

	U32 alignUp(U32 value, U32 align);
} // namespace rat::cc::detail

#endif
