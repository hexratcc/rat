#ifndef RAT_CC_EMIT_H
#define RAT_CC_EMIT_H

#include "emit/emit_state.h"
#include "parse/const_eval.h"
#include "target_layout.h"

namespace rat::cc {
	namespace detail {
		U32 alignedChunkWidth(U32 offset, U32 size);
		B32 arrayParts(CType t, CType& elem, U32& count);
		void writeLittleEndian(List<U8>& out, U32 off, U64 value, U32 size);
		AbiClass mergeAbiClass(AbiClass a, AbiClass b);
		void classifyBits(AbiClass (&cls)[2], U64 bitOffset, U64 bits, AbiClass c);
		Pair<String, DebugType::Kind> debugBaseName(CType t);
		B32 fpBuiltinSuffix(const String& name, const C8* stem);
		U32 bitBuiltinWidth(const String& name, const C8* stem, U32 longBits);
		B32 isKnownBuiltin(const String& name);

		// a parsed __builtin_*_overflow name
		struct OvfForm {
			Opcode op = Opcode::Add;
			B32 predicate = false;
			B32 typed = false;
			CType fixed;
		};

		B32 stemOp(const String& s, U64 pos, Opcode& op);
		B32 parseOverflowName(const String& name, U32 longBits, OvfForm& out);
		U32 signedWidth(CType t);
		B32 exactInSigned64(Opcode op, CType a, CType b);
		B32 exactInUnsigned64(Opcode op, CType a, CType b);
		B32 isLabeledStmt(const Stmt* s);
		B32 isRegConstraint(const String& c, B32 isOutput, B32& readWrite);
		B32 matchingConstraint(const String& c, U32& index);

		// shared child traversal for AST scans
		struct AstWalk {
			virtual ~AstWalk() = default;

			// child edges this walk follows
			B32 sizeofOperand = false;	 // operand of sizeof
			B32 alignOfOperand = false;	 // operand of _Alignof
			B32 compoundLitInit = false; // initializer of a compound literal
			B32 stmtExprBody = false;		 // body of a statement expression
			B32 exprChildren = false;		 // expressions a statement holds
			B32 nestedSwitch = false;		 // children of a nested switch
			B32 forInit = false;				 // initializer statement of a for

			// per-node action, run before the children
			virtual B32 onExpr(const Expr*) { return true; }
			virtual B32 onStmt(const Stmt*) { return true; }
		};

		B32 walkExpr(AstWalk& w, const Expr* e);
		B32 walkExprChildren(AstWalk& w, const Expr* e);
		B32 walkStmt(AstWalk& w, const Stmt* s);
		B32 walkStmtChildren(AstWalk& w, const Stmt* s);
	} // namespace detail

	String builtinLibcName(const String& name);

	struct Emitter {
		Emitter(Module& module, const TargetLayout& layout);

		B32 emit(const TransUnit& unit);
		// describe the unit for debuggers, locations come from ts
		void enableDebug(const TokenStream& ts, B32 keepLocalsInMemory, B32 describeVars);

		const String& error() const { return diag.message; }
		const List<String>& warnings() const { return diag.warnings; }
	private:
		// values
		struct Value {
			Node* node = nullptr;
			CType type;
		};
		struct Wide {
			Node* lo = nullptr;
			Node* hi = nullptr;
		};
		struct LValue {
			enum class Kind : U8 {
				Var,
				Addr
			};
			Kind kind = Kind::Addr;
			Function::Var var = 0;
			Node* addr = nullptr;
			CType type;
			B32 isBitfield = false;
			U32 bitWidth = 0;
			U32 bitOffset = 0;

			B32 isVar() const { return kind == Kind::Var; }
		};
		struct CaseSet {
			Node* val;
			CType ct;
			List<I64> values;
			List<Block*> blocks;
			Block* miss;
		};

		// call signatures
		struct Callee {
			Node* target = nullptr;
			const FuncType* ft = nullptr;
			B32 direct = false;
		};
		// argument registers still free, sysv
		struct ArgRegs {
			U32 gp = rat::abi::kSysV.gpArgCount;
			U32 sse = rat::abi::kSysV.sseArgCount;
		};
		struct CallArgs {
			List<Node*> nodes;
			CallAbi abi;
			ArgRegs free;
		};

		// the shared folder with the sizes only the emitter knows, see sizeofOperand
		struct ConstFolder final : ConstEval {
			explicit ConstFolder(Emitter& e)
			: ConstEval(e.lay),
				emit(e) {}
			B32 sizeOf(const Expr* e, U64& out) const override;
			Emitter& emit;
		};

		// initializer sinks
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
			B32 store(U32 off, LValue lv, const Expr* e);
			Emitter& emit;
			Function& fn;
			Node* slot;
		};
		// dry run sink, see initArrayCount
		struct ExtentSink final : InitSink {
			B32 scalar(U32 off, CType dt, const Expr* e) override;
			B32 bitfield(U32 off, CType dt, U32 width, U32 bitOff, const Expr* e) override;
			B32 charArray(U32 base, CType elem, U32 count, const Expr* e) override;
			B32 structCopy(U32 off, CType ty, const Expr* e) override;
		};
		struct FlatList {
			const List<Expr*>& els;
			U32& pos;
			const List<Designator>* des = nullptr;
		};

		// functions
		B32 emitFunctionBody(const FuncDef* def);
		List<AbiRegs> lowerParams(const FuncDef* def, List<Type*>& paramTypes, CallAbi& abi);
		void
		bindFunctionParams(Function& fn, const FuncDef* def, U32 paramBase, const List<AbiRegs>& regs);
		Node* bindEightbytes(Function& fn, CType t, const AbiRegs& regs, U32& index);
		void markRestrictParam(Function& fn, Function::Var v, U32 index);
		void dropRestrictParam(Function& fn, Function::Var v);
		B32 checkAliases();

		// types and sizes
		Type* irType(CType t);
		Type* byteArrayType(U32 n);
		CType funcPtrType(const FunctionSignature& sig);
		U64 byteSize(CType t) const;
		U32 alignOf(CType t) const;
		Symbol lookupSymbol(const String& name);
		void memberField(const Expr* e, CType& structType, const Field*& field);
		B32 sizeofOperand(const Expr* operand, U64& out);

		// memory and conversions
		Node* constSize(Function& fn, U64 value);
		Node* allocBytes(Function& fn, U32 size, U32 align = 0);
		Node* declSlot(Function& fn, const Declarator& d, Type* ty, U32 size);
		Node* emitArrayByteSize(Function& fn, CType t);
		Node* vlaDimCount(Function& fn, CType arr);
		B32 bindVlaDims(Function* fn, CType t);
		B32 arrayBound(CType arr, I64& count);
		CType completeArray(CType arr, U64 count);
		B32 compoundArrayCount(const Expr* e, I64& count);
		Node* convert(Function& fn, Node* n, CType from, CType to);
		Node* reduceBitfield(Function& fn, Node* n, CType t);
		Node* toBool(Function& fn, const Value& v);
		Node* fromBool(Function& fn, Node* boolean);
		Node* offsetPtr(Function& fn, Node* base, U64 byteOff);
		Node* elemStride(Function& fn, CType ptrType);
		void emitMemCopy(Function& fn, Node* dst, Node* src, U32 size);

		// lvalues
		B32 emitLValue(Function& fn, const Expr* e, LValue& out);
		B32 emitMemberLValue(Function& fn, const Expr* e, LValue& out);
		B32 emitCompoundLitLValue(Function& fn, const Expr* e, LValue& out);
		Node* loadLValue(Function& fn, const LValue& lv);
		void storeLValue(Function& fn, const LValue& lv, Node* value);

		// diagnostics
		Failed failUndeclared(const String& name) {
			return diag.fail("use of undeclared identifier '" + name + "'");
		}
		Failed failArrayAssign(const Expr* target) {
			if(target->kind == ExprKind::Ident)
				return diag.fail("array '" + *target->ident.name + "' is not assignable");
			return diag.fail("array is not assignable");
		}
		Failed failArrayCount() { return diag.fail("array size must be a positive integer constant"); }
		Failed failArrayUnknownSize(const String& name) {
			return diag.fail("array '" + name + "' has unknown size");
		}
		Failed failTooManyInits() { return diag.fail("too many initializers for the array"); }
		Failed failNonConstInit() {
			return diag.fail("initializer element is not a constant expression");
		}
		Failed failStringNeedsCharArray() {
			return diag.fail("string initializer requires a 'char' array");
		}

		// statements
		B32 emitStmt(Function& fn, const Stmt* stmt);
		B32 emitCompound(Function& fn, const Stmt* stmt);
		B32 emitCaseLabel(Function& fn, const Stmt* stmt);
		B32 emitIf(Function& fn, const Stmt* stmt);
		B32 emitWhile(Function& fn, const Stmt* stmt);
		B32 emitLoopBody(Function& fn, const Stmt* body, LoopFrame& frame, Block* next);
		B32 emitDoWhile(Function& fn, const Stmt* stmt);
		B32 emitFor(Function& fn, const Stmt* stmt);
		B32 emitForScoped(Function& fn, const Stmt* stmt);
		B32 emitSwitch(Function& fn, const Stmt* stmt);
		B32 emitCaseTable(Function& fn, const CaseSet& cs);
		void emitCaseTree(Function& fn, const CaseSet& cs, U32 lo, U32 hi);
		B32 emitReturn(Function& fn, const Expr* value);
		B32 emitLabel(Function& fn, const Stmt* stmt);
		B32 emitGoto(Function& fn, const Stmt* stmt);
		B32 emitAsm(Function& fn, const Stmt* stmt);
		B32 asmFitsRegister(CType t) const;
		B32 emitCondBranch(Function& fn, const Expr* cond, Block* trueB, Block* falseB);
		B32 declMayBeVla(const Declarator& d);
		B32 stmtHasVla(const Stmt* s);
		B32 blockDeclaresVla(const Stmt* s);
		void restoreStack(Function& fn, Node* sp);

		// AST scans
		void collectAddrTaken(const Stmt* s);
		void collectLabels(Function& fn, const Stmt* s);
		static B32 containsLabel(const Stmt* s);
		static B32 containsSwitchCase(const Stmt* s);
		void collectSwitchCases(const Stmt* s, List<const Stmt*>& cases, const Stmt*& def);
		B32 exprRefersTo(const Expr* expr, const String& name) const;

		// declarations
		B32 emitOneDecl(Function& fn, const Declarator& d, B32 live);
		B32 emitComplexDecl(Function& fn, const Declarator& d, B32 live);
		B32 emitStructDecl(Function& fn, const Declarator& d, B32 live);
		B32 emitArrayDecl(Function& fn, const Declarator& d, B32 live);
		B32 emitStringArrayDecl(Function& fn, const Declarator& d, I64 count, B32 live);
		B32 emitVlaDecl(Function& fn, const Declarator& d);
		B32 declareStatic(Function& fn, const Declarator& d);
		B32 declareExtern(Function& fn, const Declarator& d);
		B32 declareDead(Function& fn, const Stmt* stmt);
		void zeroSlot(Function& fn, Node* slot, U32 size);

		// expressions
		Value emitExpr(Function& fn, const Expr* expr);
		Value emitUnary(Function& fn, const Expr* expr);
		Value emitAddrOf(Function& fn, const Expr* expr);
		Value emitDeref(Function& fn, const Expr* expr);
		Value loadObject(Function& fn, Node* addr, CType type);
		Value emitIdent(Function& fn, const Expr* expr);
		Value symbolValue(Function& fn, const String& name, const Symbol& sym);
		Value emitSizeof(Function& fn, const Expr* expr);
		Value emitStmtExpr(Function& fn, const Expr* expr);
		Value emitTernary(Function& fn, const Expr* expr);
		Value emitCompoundLit(Function& fn, const Expr* expr);
		Value emitAssign(Function& fn, const Expr* expr);
		Value assignStruct(Function& fn, const Expr* e, const LValue& lv, Value rhs);
		Value assignComplex(Function& fn, const Expr* e, const LValue& lv, Value rhs);
		Value emitIncDec(Function& fn, const Expr* expr);
		Value emitBinary(Function& fn, const Expr* expr);
		Value emitComplexBinary(Function& fn, ExprOp op, Value lhs, Value rhs);
		Value emitArithValue(Function& fn, ExprOp op, Value lhs, Value rhs, CType lt, CType rt);
		Value emitLogicalBinary(Function& fn, const Expr* expr);
		Value emitComparison(Function& fn, ExprOp op, Value lhs, Value rhs);
		Value emitPtrArith(Function& fn, ExprOp op, Value lhs, Value rhs);
		Node* emitArith(Function& fn, ExprOp op, Node* l, Node* r, CType ct);

		// complex numbers
		Node* complexReal(Function& fn, const Value& v);
		Node* complexImag(Function& fn, const Value& v);
		Value makeComplex(Function& fn, CType type, Node* re, Node* im);
		Value toComplex(Function& fn, const Value& v, CType type);
		Value complexBinary(Function& fn, ExprOp op, Value lhs, Value rhs, CType ct);
		void storeComplex(Function& fn, Node* addr, CType type, const Value& v);

		// calls
		Value emitCall(Function& fn, const Expr* expr);
		B32 resolveCallee(Function& fn, const Expr* e, Callee& out);
		const FuncType* implicitFuncType(const String& callee);
		B32 emitArgs(Function& fn, const Expr* e, const Callee& c, U32 n, CallArgs& args);
		void pushAggregateArg(Function& fn, Node* src, CType t, CallArgs& args);
		Value emitVaArgAggregate(Function& fn, Node* ap, CType t);

		// x86-64 aggregate abi
		AbiRegs classify(CType t) const;
		AbiRegs argRegs(CType t) const;
		AbiRegs returnRegs(CType t) const;
		void classifyEightbytes(CType t, U64 offset, AbiClass (&cls)[2]) const;
		void classifyFields(const StructType* st, U64 offset, AbiClass (&cls)[2]) const;
		AbiRegs takeArgRegs(CType t, ArgRegs& free) const;
		ArgRegs freeArgRegs(CType ret) const;
		void markByval(CallAbi& abi, U32 index, CType t) const;
		Type* eightbyteType(AbiClass c);

		// builtins
		using BuiltinEmitter = B32 (Emitter::*)(Function& fn, const Expr* expr, Value& out);
		B32 emitBuiltinCall(Function& fn, const Expr* expr, Value& out);
		B32 emitVaBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitVaCopyBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitAllocaBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitJmpBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitExpectBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitConstantPBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitTrapBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitBitCountBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitOverflowBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitAbsBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitBswapBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitClassifyBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitFpClassBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitFrameBuiltin(Function& fn, const Expr* expr, Value& out);
		B32 emitPrefetchBuiltin(Function& fn, const Expr* expr, Value& out);
		Value voidValue(Function& fn);
		Node* vaListRef(Function& fn, const Expr* ap);
		B32 oneArg(const Expr* e);
		Value emitOneArg(Function& fn, const Expr* e);
		B32 failBuiltin(const String& msg);
		Node* wideHigh(Function& fn, Node* lo, B32 isUnsigned);
		Wide wideExtend(Function& fn, Node* v, CType from);
		Wide wideAddSub(Function& fn, Wide a, Wide b, B32 sub);
		Wide wideMul(Function& fn, Wide a, Wide b);
		Node* wideFits(Function& fn, Wide v, CType rt);
		Node* fitsIn64(Function& fn, Node* r, CType rt, B32 sgn);

		// constants
		B32 evalAddrConst(const Expr* expr, String& symbol, I64& addend);
		B32 addrConstOf(const Expr* lv, String& symbol, I64& addend);

		// globals
		String internString(const Expr* strLit);
		Node* floatLit(Function& fn, CType t, F80 v);
		B32 internCompoundLiteral(const Expr* compound, String& outSym);
		B32 registerGlobals(const TransUnit& unit);
		B32 registerGlobalAlias(const Declarator& d);
		B32 registerGlobal(const Declarator& d, const String& sym, Function* fn);
		B32 registerGlobalArray(const Declarator& d, const String& sym, Function* fn);
		B32 registerGlobalStruct(const Declarator& d, const String& sym, Function* fn);
		B32 registerGlobalScalar(const Declarator& d, const String& sym, Function* fn);
		void defineGlobal(const Declarator& d, const String& sym, Type* ty, List<U8>&& img);
		void bindGlobal(const Declarator& d, const String& sym, Function* fn, CType type);

		// debug info, the hooks do nothing without -g
		SrcLoc debugLoc(U32 tok);
		DebugType* debugType(CType t);
		DebugType* debugBase(CType t);
		DebugType* debugRecord(const StructType* st);
		DebugType* debugNamed(const NamedType* n);
		DebugType* debugWrap(DebugType::Kind kind, DebugType* elem);
		void debugFunction(Function& fn, const FuncDef* def);
		void debugSetLoc(Function& fn, U32 tok);
		U32 debugEnterScope(Function& fn, const Stmt* s);
		void debugVar(Function& fn, DebugVar v, const Local& l, B32 hasValue);
		void debugLocal(Function& fn, const Declarator& d);
		void debugGlobal(const Declarator& d);

		// initializers
		static const Expr* peelAggregateCompound(const Expr* el);
		Expr* wrapNested(const Designator* sub, const Expr* val);
		I64 fieldIndex(const StructType* st, const Designator& des);
		const StructType* anonGroupType(const StructType* st, U32 firstIdx);
		B32 unwrapScalarInit(const Expr*& e, B32& skip);
		B32 initListIsFlat(const Expr* init);
		U32 initArrayCount(CType elem, const Expr* init);
		B32 initStructInit(InitSink& sink,
											 U32 base,
											 const StructType* st,
											 const Expr* init,
											 U32* flexExtent = nullptr);
		B32 initUnionInit(InitSink& sink, U32 base, const StructType* st, const Expr* init);
		B32 initArrayInit(
				InitSink& sink, U32 base, CType elem, U32 count, const Expr* init, U32* extent = nullptr);
		B32 initArrayRow(InitSink& sink, U32 off, CType elem, const Expr* init, U32& i);
		B32 initFlatObject(InitSink& sink, U32 base, CType ty, const FlatList& src);
		B32 initFlatStruct(InitSink& sink, U32 base, const StructType* st, const FlatList& src);
		B32 initFlatArray(InitSink& sink,
											U32 base,
											CType elem,
											U32 count,
											const FlatList& src,
											U32* extent = nullptr);
		B32 initFlatArrayObject(InitSink& sink, U32 base, CType elem, U32 count, const FlatList& src);
		B32 checkStringElem(CType elem, U32 charSize);
		U32 flexElemCount(const StructType* st, const Expr* init);
		void encodeFloatBytes(CType dt, F80 v, List<U8>& out);
		B32 writeConstScalar(List<U8>& img, U32 off, CType dt, const Expr* e, B32 addrFits);
	private:
		// module and target
		Module& mod;
		TargetLayout lay;
		Arena arena;
		Type* i32 = nullptr;
		// state
		Diagnostics diag;
		FunctionState func;
		Symbols syms;
		GlobalData data;
		DebugState dbg;
		ConstFolder folder{*this};
	};
} // namespace rat::cc

#endif
