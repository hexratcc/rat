#ifndef RAT_CC_EMIT_H
#define RAT_CC_EMIT_H

#include "Ast.h"

#include "IR/Function.h"
#include "IR/Module.h"

namespace rat {
	struct Node;
	struct Type;
} // namespace rat

namespace rat::cc {
	struct Emitter {
		explicit Emitter(Module& module);

		B32 emit(const TransUnit& unit);

		const String& error() const { return errMsg; }
	private:
		struct Value {
			Node* node = nullptr;
			CType type;
		};

		B32 emitStmt(Function& fn, const Stmt* stmt);
		B32 emitFunctionBody(const FuncDef* def);
		void bindFunctionParams(Function& fn, const FuncDef* def, U32 paramBase);
		B32 declareDead(Function& fn, const Stmt* stmt);
		Value emitExpr(Function& fn, const Expr* expr);
		Value emitAssign(Function& fn, const Expr* expr);
		Value emitIncDec(Function& fn, const Expr* expr);
		Value emitBinary(Function& fn, const Expr* expr);
		Value emitLogicalBinary(Function& fn, const Expr* expr);
		Value emitComparison(Function& fn, ExprOp op, Value lhs, Value rhs);
		Value emitTernary(Function& fn, const Expr* expr);
		Value emitTernarySelect(Function& fn, const Expr* expr);
		Value emitCompoundLit(Function& fn, const Expr* expr);
		Value emitCall(Function& fn, const Expr* expr);
		Value emitUnary(Function& fn, const Expr* expr);
		Value emitAddrOf(Function& fn, const Expr* expr);
		Value emitDeref(Function& fn, const Expr* expr);
		Value emitIdent(Function& fn, const Expr* expr);
		Value emitSizeof(Function& fn, const Expr* expr);
		Value emitStmtExpr(Function& fn, const Expr* expr);
		B32 emitBuiltinCall(Function& fn, const Expr* expr, Value& out);
		Node* emitArith(Function& fn, ExprOp op, Node* l, Node* r, CType ct);
		CType completeComplex(CType t);
		Node* complexReal(Function& fn, const Value& v);
		Node* complexImag(Function& fn, const Value& v);
		Value makeComplex(Function& fn, CType type, Node* re, Node* im);
		Value toComplex(Function& fn, const Value& v, CType type);
		Value emitComplexBinary(Function& fn, ExprOp op, Value lhs, Value rhs, CType ct);
		Value emitComplexUnary(Function& fn, ExprOp op, Value v);
		void storeComplex(Function& fn, Node* addr, CType type, const Value& v);
		B32 emitDecl(Function& fn, const Stmt* stmt);
		B32 emitOneDecl(Function& fn, const Declarator& d);
		B32 emitComplexDecl(Function& fn, const Declarator& d);
		B32 emitStructDecl(Function& fn, const Declarator& d);
		B32 emitArrayDecl(Function& fn, const Declarator& d);
		B32 emitTypedefArrayDecl(Function& fn, const Declarator& d);
		B32 emitMultiDimArrayDecl(Function& fn, const Declarator& d);
		B32 declareStatic(Function& fn, const Declarator& d);
		B32 emitVlaDecl(Function& fn, const Declarator& d);
		B32 declIsVla(const Declarator& d, I64& count);
		void zeroSlot(Function& fn, Node* slot, U32 size);
		B32 emitCompound(Function& fn, const Stmt* stmt);
		B32 emitCaseLabel(Function& fn, const Stmt* stmt);
		B32 emitIf(Function& fn, const Stmt* stmt);
		B32 emitWhile(Function& fn, const Stmt* stmt);
		B32 emitDoWhile(Function& fn, const Stmt* stmt);
		B32 emitFor(Function& fn, const Stmt* stmt);
		B32 emitSwitch(Function& fn, const Stmt* stmt);
		B32 emitReturn(Function& fn, const Stmt* stmt);
		B32 emitExprStmt(Function& fn, const Stmt* stmt);
		B32 emitLabel(Function& fn, const Stmt* stmt);
		B32 emitGoto(Function& fn, const Stmt* stmt);
		void collectSwitchCases(const Stmt* s, List<const Stmt*>& cases, const Stmt*& def);
		B32 evalConst(const Expr* expr, I64& out);
		B32 evalConstTyped(const Expr* expr, I64& out, CType& ty);
		B32 evalConstUnary(ExprOp op, I64 v, CType opTy, I64& out, CType& ty);
		B32 evalConstBinary(ExprOp op, I64 a, CType aTy, I64 b, CType bTy, I64& out, CType& ty);
		B32 evalFloatConst(const Expr* expr, long double& out);
		void encodeFloatBytes(CType dt, long double v, List<U8>& out);
		B32 evalAddrConst(const Expr* expr, String& symbol, I64& addend);
		B32 addrConstOf(const Expr* lv, String& symbol, I64& addend);
		String internString(const Expr* strLit);
		B32 internCompoundLiteral(const Expr* compound, String& outSym);

		B32 resolveType(CType& t);
		B32 typeOf(const Expr* expr, CType& out);
		B32 typeOfUnary(const Expr* expr, CType& out);
		B32 typeOfBinary(const Expr* expr, CType& out);

		const Expr* genericSelect(const Expr* e);
		B32 exprRefersTo(const Expr* expr, const String& name) const;
		Node* convert(Function& fn, Node* n, CType from, CType to);
		Type* irType(CType t);
		U32 byteSize(CType t) const;
		CType ctSize() const;
		CType ctPtrDiff() const;
		Node* constSize(Function& fn, U64 value);
		Node* allocBytes(Function& fn, U32 size);
		B32 sizeofOperand(const Expr* operand, U32& out);
		Node* emitArrayElemCount(Function& fn, CType t);
		Node* emitArrayByteSize(Function& fn, CType t);

		struct LValue {
			B32 isVar = false;
			Function::Var var = 0;
			Node* addr = nullptr;
			CType type;
			B32 isArray = false;
			B32 isBitfield = false;
			U32 bitWidth = 0;
			U32 bitOffset = 0;
		};

		B32 emitLValue(Function& fn, const Expr* e, LValue& out);
		B32 emitMemberLValue(Function& fn, const Expr* e, LValue& out);
		B32 emitCompoundLitLValue(Function& fn, const Expr* e, LValue& out);
		Value emitStructAssign(Function& fn, const Expr* e, const LValue& lv, Value rhs);
		Value emitComplexAssign(Function& fn, const Expr* e, const LValue& lv, Value rhs);
		Node* loadLValue(Function& fn, const LValue& lv);
		void storeLValue(Function& fn, const LValue& lv, Node* value);

		void emitMemCopy(Function& fn, Node* dst, Node* src, U32 size);
		Node* offsetPtr(Function& fn, Node* base, U64 byteOff);
		Node* elemStride(Function& fn, CType ptrType);
		Value emitPtrArith(Function& fn, ExprOp op, Value lhs, Value rhs);

		Node* toBool(Function& fn, const Value& v);
		Node* fromBool(Function& fn, Node* boolean);

		struct Local {
			Function::Var var = 0;
			Node* addr = nullptr;
			CType type;
			B32 inMem = false;
			B32 isArray = false;
			U32 count = 0;
			Node* lengthNode = nullptr;
		};
		void pushScope();
		void popScope();
		void declare(const String& name, Local local);
		B32 lookup(const String& name, Local& out) const;

		void fail(const String& msg);
		void failUndeclared(const String& name) { fail("use of undeclared identifier '" + name + "'"); }
		void failArrayCount() { fail("array size must be a positive integer constant"); }
		void failArrayUnknownSize(const String& name) { fail("array '" + name + "' has unknown size"); }
		void failFieldInArray() { fail("field designator in an array initializer"); }
		void failTooManyInits() { fail("too many initializers for the array"); }
		void failScalarInit() { fail("invalid initializer for a scalar"); }
		void failNonConstInit() { fail("initializer element is not a constant expression"); }
		void failStringNeedsCharArray() { fail("string initializer requires a 'char' array"); }

		struct LoopFrame {
			Function::Block* brk = nullptr;
			Function::Block* cont = nullptr;
			B32 exitReachable = false;
			B32 isSwitch = false;
		};

		struct FnSig {
			CType ret;
			List<CType> params;
			B32 isVarArgs = false;
			B32 unprototyped = false;
		};

		struct Callee {
			Node* target = nullptr;
			const FuncType* ft = nullptr;
			FnSig sig;
			B32 direct = false;
			B32 prototyped = false;
		};

		B32 resolveCallee(Function& fn, const Expr* e, Callee& out);
		B32 emitCallArgs(Function& fn, const Expr* e, const Callee& c, U32 nparams, List<Node*>& args);

		CType funcPtrType(const FnSig& sig);
		List<Reloc> relocs;
		Module& mod;
		Arena arena;
		Type* i32 = nullptr;
		B32 failed = false;
		String errMsg;
		U32 curOffset = 0;
		U32 flexCount = 0;
		String curFunc;
		List<Map<String, Local>> scopes;
		List<LoopFrame> loops;
		List<Map<const Stmt*, Function::Block*>> switches;
		Map<String, FnSig> funcs;
		Map<String, CType> globals;
		Map<String, CType> globalArrays;
		Map<String, U32> globalArrayCounts;
		Map<U32, StructType*> complexLayouts;
		CType curRet;
		Node* sretSlot = nullptr;

		Set<String> memVars;
		void collectAddrTaken(const Stmt* s);
		void collectAddrTakenExpr(const Expr* e);

		U32 strCounter = 0;
		Node* emitStringLiteral(Function& fn, const Expr* e);

		Map<String, Function::Block*> labelBlocks;
		void collectLabels(Function& fn, const Stmt* s);
		void collectLabelsInExpr(Function& fn, const Expr* e);
		static B32 containsLabel(const Stmt* s);
		static B32 containsLabelInExpr(const Expr* e);
		static B32 containsSwitchCase(const Stmt* s);

		B32 registerGlobals(const TransUnit& unit);
		B32 registerGlobalArray(const Declarator& d, const String& symbol, Function* fn);
		B32 validateGlobalArrayLen(const Declarator& d, I64& count, B32& haveLen);
		B32 registerGlobalArrayOfArray(const Declarator& d, const String& symbol, Function* fn);
		B32 registerGlobalArrayOfStruct(const Declarator& d, const String& symbol, Function* fn);
		B32 registerGlobalArrayOfScalar(const Declarator& d, const String& symbol, Function* fn);
		void bindArrayGlobal(const Declarator& d, const String& symbol, Function* fn, U32 count);
		B32 registerGlobalStruct(const Declarator& d, const String& symbol, Function* fn);
		B32 registerGlobalScalar(const Declarator& d, const String& symbol, Function* fn);
		U32 staticCounter = 0;
		I64 fieldIndex(const StructType* st, const Designator& des);
		const StructType* anonGroupType(const StructType* st, U32 firstIdx);

		const Expr* wrapNested(const Designator* sub, const Expr* val);
		static const Expr* peelAggregateCompound(const Expr* el);

		struct InitSink {
			virtual ~InitSink() = default;
			virtual B32 scalar(U32 off, CType dt, const Expr* e) = 0;
			virtual B32 bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) = 0;
			virtual B32 charArray(U32 base, CType elem, U32 count, const Expr* e) = 0;
			virtual B32 structCopy(U32 off, CType ty, const Expr* e) = 0;
		};
		struct ImageSink final : InitSink {
			ImageSink(Emitter& e, List<U8>& image)
			: emit(e),
				img(image) {}
			B32 scalar(U32 off, CType dt, const Expr* e) override;
			B32 bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) override;
			B32 charArray(U32 base, CType elem, U32 count, const Expr* e) override;
			B32 structCopy(U32 off, CType ty, const Expr* e) override;
			Emitter& emit;
			List<U8>& img;
		};
		struct StoreSink final : InitSink {
			StoreSink(Emitter& e, Function& f, Node* s)
			: emit(e),
				fn(f),
				slot(s) {}
			B32 scalar(U32 off, CType dt, const Expr* e) override;
			B32 bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) override;
			B32 charArray(U32 base, CType elem, U32 count, const Expr* e) override;
			B32 structCopy(U32 off, CType ty, const Expr* e) override;
			Emitter& emit;
			Function& fn;
			Node* slot;
		};

		B32 storeScalar(Function& fn, Node* slot, U32 off, CType dt, const Expr* e);
		B32 storeCharArray(Function& fn, Node* slot, U32 base, CType elem, U32 count, const Expr* e);

		B32 initStructInit(InitSink& sink, U32 base, const StructType* st, const Expr* init);
		B32 initUnionInit(InitSink& sink, U32 base, const StructType* st, const Expr* init);
		B32 initArrayInit(InitSink& sink, U32 base, CType elem, U32 count, const Expr* init);
		B32 initArrayRow(InitSink& sink,
										 U32 off,
										 CType elem,
										 const Expr* init,
										 const Designator& des,
										 U32& i,
										 U32& cur);
		U32 arrayInitOuterExtent(CType elem, const Expr* init);
		U32 scalarLeaves(CType ty);
		B32 initFlatObject(InitSink& sink,
											 U32 base,
											 CType ty,
											 const List<Expr*>& els,
											 U32& pos,
											 const List<Designator>* des = nullptr);
		B32 initFlatStruct(InitSink& sink,
											 U32 base,
											 const StructType* st,
											 const List<Expr*>& els,
											 U32& pos,
											 const List<Designator>* des = nullptr);
		B32 initFlatArray(InitSink& sink,
											U32 base,
											CType elem,
											U32 count,
											const List<Expr*>& els,
											U32& pos,
											const List<Designator>* des = nullptr);
		U32 flatArrayCount(CType elem, const List<Expr*>& els);
		void flatConsumeObject(CType ty, const List<Expr*>& els, U32& pos);
		U32 flexElemCount(const StructType* st, const Expr* init);
	};
} // namespace rat::cc

#endif
