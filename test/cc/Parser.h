#ifndef RAT_CC_PARSER_H
#define RAT_CC_PARSER_H

#include "Ast.h"
#include "Lexer.h"

namespace rat::cc {
	struct Parser {
		Parser(Lexer& lexer, Arena& arena);

		TransUnit* parseUnit();

		B32 ok() const { return !failed; }
		const String& error() const { return errMsg; }

	private:
		// token cursor
		const Token& peek() { return lex.peek(); }
		Token advance() { return lex.next(); }
		B32 check(TokKind kind) { return peek().kind == kind; }
		B32 accept(TokKind kind);
		B32 expect(TokKind kind, const char* what);
		void fail(const Token& at, const String& msg);

		// grammar
		FuncDef* parseFunction();
		Stmt* parseCompound();
		Stmt* parseStatement();
		Stmt* parseDeclaration();

		Expr* parseExpression();
		Expr* parseAssignment();
		Expr* parseConditional();
		Expr* parseBinary(I32 minPrec);
		Expr* parseUnary();
		Expr* parsePrimary();

		// node builders
		Expr* makeExpr(ExprKind kind, U32 offset);
		Expr* makeInt(const Token& tok, I64 value, B32 isUnsigned, B32 isLong);
		Expr* makeIdent(const Token& tok);
		Expr* makeUnary(U32 offset, ExprOp op, Expr* operand);
		Expr* makeBinary(U32 offset, ExprOp op, Expr* lhs, Expr* rhs);

		B32 parseIntLiteral(const Token& tok, I64& value, B32& isUnsigned,
												B32& isLong);

		Lexer& lex;
		Arena& arena;
		B32 failed = false;
		String errMsg;
	};
} // namespace rat::cc

#endif
