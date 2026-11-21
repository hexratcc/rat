#ifndef RAT_CC_PARSER_H
#define RAT_CC_PARSER_H

#include "lex/token_stream.h"
#include "parse/ast.h"
#include "parse/const_eval.h"
#include "target_layout.h"

namespace rat::cc {
	struct Parser {
		Parser(TokenStream& lexer, Arena& arena, const TargetLayout& layout);

		TransUnit* parseUnit();

		const String& error() const { return errMsg; }
	private:
		// token cursor
		const Token& peek() { return lex.peek(); }
		const Token& peek2() { return lex.peek2(); }
		Token advance() { return lex.next(); }
		B32 check(TokKind kind) { return peek().kind == kind; }
		B32 accept(TokKind kind);
		B32 expect(TokKind kind, const C8* what = nullptr);
		Failed fail(const Token& at, const String& msg);

		// type specifiers
		// the keywords of one type-spec, counted by kind
		struct DeclSpecs {
			// queries
			U32 of(TokKind k) const { return count[(U32)k]; }
			B32 has(TokKind k) const { return of(k) != 0; }

			// state
			U8 count[(U32)TokKind::KwAlignas + 1] = {};
			U32 typeWordCount = 0;
			U32 storageCount = 0;
			U32 align = 0; // raised by alignas
		};
		B32 startsType(const Token& tok);
		B32 parseTypeSpec(CType& out);
		B32 parseQualStorage(DeclSpecs& seen);
		B32 parseTypeKeywords(DeclSpecs& seen);
		CType basicType(const DeclSpecs& seen);
		B32 finishTypeSpec(DeclSpecs seen, CType& out);
		B32 parseAlignasSpec(U32& align);
		B32 acceptTrailingAlignas(U32& align);
		B32 parseTypeofSpec(CType& out);
		B32 parseEnumSpec(CType& out);
		B32 parseEnumerators(NamedType& e);
		B32 parseStructSpec(CType& out);
		struct StructLayout {
			U64 offset = 0; // bytes used so far, for a union its widest member
			U32 align = 1;
			U32 bitPos = 0; // next free bit of a struct
		};
		B32 parseStructBody(StructType* st, B32 isUnion);
		void spliceAnonMember(StructType* st, const StructType* inner, B32 isUnion, StructLayout& l);
		B32 parseStructMember(StructType* st, CType base, U32 baseAlign, B32 isUnion, StructLayout& l);
		B32 arrayMemberCount(CType& t);
		B32 parseBitfield(StructType* st, Field f, U32 memberAlign, B32 isUnion, StructLayout& l);
		U64 placeMember(U64 size, U32 align, B32 isUnion, StructLayout& l);
		B32 parseTypedef(Stmt* s);
		B32 parseTypeName(CType& out);

		// declarators
		struct DeclOp {
			enum class Kind : U8 {
				Pointer,
				Array,
				Func
			};
			Kind kind = Kind::Pointer;
			U64 count = 0;						// elements of an Array (0 when not constant)
			Expr* bound = nullptr;		// bound of an Array as written
			FuncType* func = nullptr; // parameter list of a Func
			CType pointers = {};			// stars and qualifiers of a Pointer, relative to the type below
		};
		struct DeclResult {
			B32 allowOldStyle = false;		// in: an old-style name list may end the declarator
			const String* name = nullptr; // null for an abstract declarator
			U32 offset = 0;								// of the name
			CType type;
			U32 align = 0; // raised by alignas inside the declarator
		};
		void parsePointers(CType& t);
		B32 parseDeclarator(CType base, DeclResult& out);
		B32 parseDeclaratorOps(List<DeclOp>& ops, DeclResult& out);
		B32 parseDirectDeclarator(List<DeclOp>& ops, DeclResult& out);
		B32 parseDeclaratorSuffixes(List<DeclOp>& ops, U32& align);
		B32 parseArrayBound(DeclOp& op);
		B32 parseParamTypeList(FuncType* ft);
		B32 parseParams(FuncType* ft);
		B32 parseParamNames(FuncType* ft);
		B32 looksLikeGroupingParen();
		CType applyDeclOps(CType base, const List<DeclOp>& ops);
		void adjustParamType(CType& t, const Expr** vlaBound = nullptr);
		void bindDeclarator(Declarator& d, const DeclResult& r);

		// declarations
		B32 parseDeclarators(CType base, const Token& start, Stmt* s, TransUnit* unit);
		B32 parseFunctionDeclarator(const DeclResult& r,
																const Token& start,
																const DeclSpecs& ds,
																TransUnit* unit,
																B32& defined);
		B32 parseObjectDeclarator(
				const DeclResult& r, const Token& start, const DeclSpecs& ds, Stmt* s, B32 fileScope);
		FuncDef* makeFuncDef(const DeclResult& r, const Token& start, const DeclSpecs& ds);
		B32 parseFunctionDef(FuncDef* fn, B32 oldStyle);
		B32 parseOldStyleDecls(FuncDef* fn);
		B32 parseDeclAttributes(const String*& aliasOut, B32& noInlineOut, U32& alignOut);
		B32 checkParamNames(const FuncDef* fn);
		B32 checkObjectComplete(const Declarator& d);
		void declare(const String& name, CType type);
		void declareObject(Declarator& d, B32 hasInit);
		void keepParamEnumerators(const FuncType* ft);
		const CType* typedefType(const String& name);
		B32 parseStaticAssert();
		Stmt* parseDeclaration(TransUnit* unit);

		// statements
		Stmt* parseCompound();
		void pushScope();
		void popScope();
		Stmt* parseStatement();
		Stmt* parseAsmStatement();
		B32 parseAsmOperands(List<AsmOperand>& out);
		B32 parseAsmStrings(List<const String*>& out);
		B32 parseAsmLabels(List<const String*>& out);
		B32 parseStrings(String& out, const C8* where);
		B32 parseAsmTemplate(const String*& out);
		Stmt* parseLabeledSub();
		Stmt* parseLabel();
		Stmt* parseGoto();
		Stmt* parseCaseLabel();
		Stmt* parseBreak();
		Stmt* parseReturn();
		B32 parseOptionalExpr(TokKind end, Expr*& out);
		Stmt* parseExprStatement();
		Expr* parseParenCond();
		Stmt* parseCondStmt(StmtKind kind);
		Stmt* parseDoWhile();
		Stmt* parseFor();
		Stmt* parseSwitch();

		// expressions
		Expr* parseExpression();
		Expr* parseInitializer();
		B32 parseInitElement(Expr* list);
		B32 parseDesignation(Designator& des, I64& rangeEnd);
		B32 parseDesignator(Designator& cur, B32 first, I64& rangeEnd);
		Expr* parseAssignment();
		Expr* parseConditional();
		Expr* parseBinary(I32 minPrec);
		Expr* parseUnary();
		Expr* parseSizeOp();
		Expr* parseCastOrCompound();
		Expr* parsePostfixTail(Expr* e);
		Expr* parsePrimary();
		Expr* parsePrimaryString();
		Expr* parsePrimaryFloat();
		Expr* parsePrimaryName();
		Expr* parsePrimaryParen();
		Expr* parseVaArg();
		Expr* parseCall(Expr* e);
		Expr* parseGeneric();
		B32 parseGenericAssoc(CType control, Expr*& selected, Expr*& fallback);
		Expr* parseBuiltinOffsetof(const Token& kw);

		static constexpr U32 kMaxParseDepth = 256;
		B32 enterDepth();
		struct DepthScope {
			Parser& p;
			explicit DepthScope(Parser& parser)
			: p(parser) {}
			~DepthScope() { --p.parseDepth; }
		};

		// node builders
		Expr* makeExpr(ExprKind kind, U32 offset);
		Stmt* makeStmt(StmtKind kind, U32 offset, Expr* expr = nullptr, Stmt* thenBody = nullptr);
		Expr* makeInt(const Token& tok, I64 value, U32 bits, U8 mods = 0);
		Expr* makeUnary(U32 offset, ExprOp op, Expr* operand);
		Expr* makeBinary(U32 offset, ExprOp op, Expr* lhs, Expr* rhs);

		// expression types, see parse_typed.cpp
		Expr* typed(Expr* e);
		CType typeOf(const Expr* e);
		CType unaryType(const Expr* e);
		CType binaryType(const Expr* e);
		CType callType(const Expr* e);
		CType memberType(const Expr* e);
		CType strType(const Expr* e);
		CType stmtExprType(const Expr* e);

		// literals
		B32 parseIntLiteral(const Token& tok, I64& value, U32& bits, U8& mods);
		U8 intLiteralMods(U64 v, B32 dec, B32 isUnsigned, U32 lCount, U32& bits) const;
		B32 parseCharLiteral(const Token& tok, I64& value);
		B32 parseStringLiteral(const Token& tok, U32 unitBytes, String& out);
		B32 decodeUcn(const String& s, U32& i, U32 end, const Token& tok, U32& cp);
		B32 decodeBackslash(const String& s, U32& i, U32 end, const Token& tok, U32 maxVal, U32& out);

		// constant folding
		B32 evalIntConst(const Expr* e, I64& out);
		U64 typeSizeBytes(CType t) const { return typeSize(t, lay.ptrBytes); }
		U32 typeAlignBytes(CType t) const { return typeAlign(t, lay.ptrBytes); }
	private:
		struct TagBinding {
			StructType* type = nullptr;
			U32 depth = 0;
		};
		// what an ordinary identifier names
		struct Ident {
			enum class Kind : U8 {
				Object,
				Typedef,
				EnumConst
			};
			Kind kind = Kind::Object;
			CType type;		 // a function has func set and ptr 0
			I64 value = 0; // of an enum constant
		};

		// input
		TokenStream& lex;
		Arena& arena;
		TargetLayout lay;

		// parse state
		U32 parseDepth = 0;
		B32 failed = false;
		String errMsg;
		String curFuncName;
		DeclSpecs specs; // storage and alignment of the last parsed type spec

		// scopes
		U32 scopeDepth = 0;
		ScopeMap<Ident> idents; // objects, functions, typedef names and enum constants
		ScopeMap<const NamedType*> enumTags;
		ScopeMap<TagBinding> structTypes;

		// unit-wide tables
		Map<String, FuncDef*> funcDefs;
		List<FuncDef*> blockProtos;
		Map<String, CType> linkedObjects; // the type of each object with linkage
	};
} // namespace rat::cc

#endif
