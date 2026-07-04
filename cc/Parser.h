#ifndef RAT_CC_PARSER_H
#define RAT_CC_PARSER_H

#include "Ast.h"
#include "Lexer.h"

#include "Target/Target.h"

#include <functional>

namespace rat::cc {
	struct Parser {
		Parser(Lexer& lexer, Arena& arena, const TargetInfo& target);

		TransUnit* parseUnit();

		B32 ok() const { return !failed; }
		const String& error() const { return errMsg; }
	private:
		// token cursor
		const Token& peek() { return lex.peek(); }
		const Token& peek2() { return lex.peek2(); }
		Token advance() { return lex.next(); }
		B32 check(TokKind kind) { return peek().kind == kind; }
		B32 accept(TokKind kind);
		B32 expect(TokKind kind, const char* what);
		void fail(const Token& at, const String& msg);

		// grammar
		void parsePointers(CType& t);
		B32 parseTypeSpec(CType& out);
		void setStorage(B32 isStatic, B32 isExtern, B32 isInline) {
			sawStatic = isStatic;
			sawExtern = isExtern;
			sawInline = isInline;
		}
		FuncDef* parseFunctionRest(CType ret,
															 const Token& nameTok,
															 const Token& start,
															 B32* moreDeclarators = nullptr);
		B32 parseOldStyleParams(FuncDef* fn);
		Stmt* parseGlobalRest(CType base, Declarator first, const Token& start);
		B32 parseSharedDeclarators(CType base, TransUnit* unit, const Token& start);
		struct Dim {
			U32 count;
			Expr* expr;
		};
		CType wrapArrayDims(CType base, const List<Dim>& dims);
		B32 parseArraySuffix(Declarator& d);
		void skipArrayQualifiers();
		B32 parseParamArray(CType& t);
		B32 looksLikeFuncPtr();
		B32 parseFuncPtrDeclarator(CType ret, Token& nameOut, CType& outType);
		void bindDeclaratorType(Declarator& d, CType t, U32 offset);
		using TypeBuilder = Delegate<CType(CType)>;
		B32 parseDeclaratorType(CType base, Token& nameOut, B32& haveName, CType& out);
		TypeBuilder parseDeclaratorBuilder(Token& nameOut, B32& haveName);
		TypeBuilder parseDirectDeclarator(Token& nameOut, B32& haveName);
		TypeBuilder parseDeclaratorSuffixes();
		B32 looksLikeGroupingParen();
		void adjustParamType(CType& t, const Expr** vlaBound = nullptr);
		B32 parseParamTypeList(FuncType* ft);
		Stmt* parseCompound();
		Stmt* parseStatement();
		B32 checkParamNames(const FuncDef* fn);
		B32 registerFuncDef(FuncDef* fn);
		Stmt* parseDeclaration();
		B32 checkObjectComplete(const Declarator& d);
		B32 parseStaticAssert();
		Stmt* parseIf();
		Stmt* parseWhile();
		Stmt* parseDoWhile();
		Stmt* parseFor();
		Stmt* parseSwitch();

		Expr* parseExpression();
		Expr* parseInitializer();
		Expr* parseAssignment();
		Expr* parseConditional();
		Expr* parseBinary(I32 minPrec);
		Expr* parseUnary();
		Expr* parsePostfix();
		Expr* parsePostfixTail(Expr* e);
		Expr* parsePrimary();
		Expr* parseGeneric();
		B32 parseTypeName(CType& out);

		// node builders
		Expr* makeExpr(ExprKind kind, U32 offset);
		Expr* makeInt(const Token& tok, I64 value, B32 isUnsigned, B32 isLong);
		Expr* makeIdent(const Token& tok);
		Expr* makeUnary(U32 offset, ExprOp op, Expr* operand);
		Expr* makeBinary(U32 offset, ExprOp op, Expr* lhs, Expr* rhs);

		B32 parseIntLiteral(const Token& tok, I64& value, B32& isUnsigned, B32& isLong);
		B32 parseCharLiteral(const Token& tok, I64& value);
		B32 parseStringLiteral(const Token& tok, String& out);
		B32 decodeEscape(const String& s, U32& i, U32 end, const Token& tok, U32 maxVal, U8& out);
		B32 decodeUcn(const String& s, U32& i, U32 end, const Token& tok, U32& cp);
		B32 parseTypeofSpec(CType& out);

		// enum support
		B32 parseEnumSpec(CType& out);
		B32 evalIntConst(const Expr* e, I64& out);
		B32 tryEvalIntConst(const Expr* e, I64& out);

		// struct/union support
		StructType* complexStruct(CType realType);
		B32 parseStructSpec(CType& out);
		B32 parseStructBody(StructType* st, B32 isUnion);
		Expr* parseBuiltinOffsetof(const Token& kw);

		// typedef support
		B32 parseTypedef();
		B32 startsType(const Token& tok);
		U32 typeSizeBytes(CType t) const { return typeSize(t, target.getPointerSizeInBytes()); }
		U32 fieldByteSize(CType t) const { return isAggregate(t) ? t.strukt->size : typeSizeBytes(t); }
		U32 fieldAlign(CType t) const { return isAggregate(t) ? t.strukt->align : typeSizeBytes(t); }

		Lexer& lex;
		Arena& arena;
		const TargetInfo& target;
		B32 failed = false;
		B32 sawStatic = false;
		B32 sawExtern = false;
		B32 sawInline = false;
		String errMsg;
		String curFuncName;
		Map<String, I64> enumConstants;
		Map<String, StructType*> structTypes;
		Map<U32, StructType*> complexLayouts;
		Map<String, CType> typedefs;
		List<FuncDef*> blockProtos;
		Map<String, FuncDef*> funcDefs;
	};
} // namespace rat::cc

#endif
