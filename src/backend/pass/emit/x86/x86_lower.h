#ifndef RAT_PASS_EMIT_X86LOWER_H
#define RAT_PASS_EMIT_X86LOWER_H

#include "core.h"

#include "codegen/schedule.h"
#include "ir/debug_info.h"
#include "ir/opcode.h"
#include "pass/emit/x86/x86_op.h"
#include "pass/pass.h"

namespace rat {
	namespace detail {
		void x86Reload(MachineInstr& m, PhysReg dst, I32 slot, U32 cls, U32 width);
		void x86Spill(MachineInstr& m, I32 slot, PhysReg src, U32 cls, U32 width);
		I32 x86AllocSlot(MachineFunc& fn, U32 cls, U32 width);
		B32 x86IsCopy(const MachineInstr& in);
	} // namespace detail

	struct X86LowerPass : MachinePass {
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
	private:
		// matched address, after its base and index are in registers
		struct AddrParts {
			VReg base = 0;
			I32 disp = 0;
			VReg index = 0;
			U32 scaleLog2 = 0;
			B32 frameBase = false; // rbp-rel
		};
		struct AddrMatch {
			Node* base = nullptr;
			Node* index = nullptr;
			Node* scaleNode = nullptr;
			U32 scaleLog2 = 0;
			I32 disp = 0;
		};
		using Kind = X86ArgAssigner::Kind;
		using Ops = MachineOperands;
		using OpList = std::initializer_list<MachineOperand>;
		struct CallArg {
			MachineOperand val; // vreg, or a 16-byte frame slot for by-value x87
			Kind cls;
			I32 reg;
		};
		// what a vreg holds above the width of its int, a bool is always 0 or 1
		enum class Ext : U8 {
			None, // undefined bits
			Sign,
			Zero
		};
		enum class ReturnMove : U8 {
			IntoSlot,
			OutOfSlot
		};
		// debug bindings of the function, grouped by the block of their anchor
		struct BindingPlan {
			List<U32> first;	 // block -> first of its bindings in order
			List<U32> order;	 // binding indexes
			List<U32> nodeEnd; // node id -> insts index past its code
			List<U32> cursor;	 // var inst -> least position left in the block
			U32 paramsEnd = 0; // past the param copies, the bool masks follow
			U32 prologueEnd = 0;
		};

		// operands and instr encodings
		static MachineOperand vr(VReg v, U32 w = 8);
		static MachineOperand ph(Reg r);
		static MachineOperand xm(U32 n, U32 w = 8);
		static MachineOperand im(I64 v, U32 w = 8);
		static MachineOperand sl(I32 s, U32 w = 8);
		static MachineOperand addrBase(const AddrParts& a);
		static I64 sibBits(B32 sign, const AddrParts& a);
		static Enc scalarEnc(U8 opc, U32 w, B32 rexW = false);
		// types and node patterns
		static B32 isX87Ty(const Type* t);
		static B32 isSseTy(const Type* t);
		static U32 intBits(const Type* t);
		static U32 gpWidth(const Type* t);
		static U32 opWidth(const Type* t);
		static U32 regWidth(const Type* t);
		static B32 repeatsLanes(const Type* t);
		static B32 readsLowLanes(const Node* n);
		static B32 gpSignOp(const Node* n);
		static B32 gpSignLoad(const LoadNode* l);
		static B32 immOf(Node* n, I64& out);
		static B32 fusableCompare(Node* n);
		static B32 selectOnlyCompare(Node* n);
		static B32 onlyUsedBy(const Node* n, Opcode op);
		static B32 sextAtDef(const Node* n);
		static B32 fillsOperand(U32 bits);
		static B32 signedOrder(const CompareNode* n);
		static Ext extFor(B32 sign);
		static I64 dwordImm(I64 v, U32 bits, B32 sign);
		static B32 splatOnlyLoad(const Node* n);
		static BinaryNode* addSubOf(const ShuffleNode* n);
		static B32 addSubOnly(const Node* n);
		static U32 log2Scale(I64 c);
		static BinaryNode* asAdd(Node* n);
		static B32 scaleOf(Node* n, Node*& idx, U32& scaleLog2);
		static AddrMatch decodeAddr(Node* ptr);
		static B32 addressOnlyAdd(Node* n);
		static B32 addressOnlyScale(Node* n);
		static List<U8> constLanes(PackNode* n, U32 esz);
		static void shuffleDwords(const ShuffleNode* n, U8& sel, U8& mask, B32& inPlace);
		static B32 isParam(const Node* v);

		// frame, vregs and values
		I32 reserve(U32 bytes, U32 align = 8);
		void needScratch();
		void layout();
		U32 classOf(const Type* t) const;
		Kind argKind(const Type* t) const;
		U64 regClobbers(B32 withCalleeSaved) const;
		VReg fresh(U32 cls);
		VReg vregFor(const Node* n);
		I32 x87SlotOf(const Node* n);
		VReg gpValue(Node* n);
		VReg sseValue(Node* n);
		VReg classValue(Node* n, U32 cls);
		I32 x87Value(Node* n);
		VReg gpConst(I64 v);
		VReg frameAddr(I64 disp);
		AddrParts matchAddr(Node* ptr);
		void loadPool(VReg d, U32 w, const String& name, const List<U8>& bytes, B32 isVec);
		void loadFp(VReg d, U64 bits, U32 w);
		void loadVec(VReg d, const List<U8>& bytes);

		// instr emitters
		MachineInstr& put(X86Op op, OpList defs, Ops uses, I64 imm = 0, I64 imm2 = 0);
		void mov(MachineOperand d, MachineOperand s);
		void moveValue(VReg dst, VReg src, U32 cls, U32 w);
		void lea(VReg d, const AddrParts& a);
		void load(X86Op op, VReg d, U32 w, const AddrParts& a, B32 sign = false);
		void store(X86Op op, const AddrParts& a, MachineOperand src);
		void alu(X86Op op, VReg d, MachineOperand rhs, U32 w = 8);
		void cmp(VReg lhs, MachineOperand rhs, U32 w = 8);
		void maskBits(VReg d, U32 bits);
		void extendBits(VReg d, U32 bits, B32 sign);
		VReg widened(VReg s, U32 bits, B32 sign);
		VReg dwordValue(Node* n, B32 sign);
		MachineInstr& sse(Enc e, MachineOperand d, Ops uses, I32 imm8 = -1);
		MachineInstr& farith(Enc e, VReg d, VReg a, VReg b, U32 w, I32 imm8 = -1);
		MachineInstr& varith(Enc e, VReg d, VReg a, VReg b, I32 imm8 = -1);
		void vmerge(Enc e, VReg d, VReg s, I32 imm8 = -1);
		void fldSlot(I32 d, I32 s);

		// node lowering
		void lowerBlocks();
		void emitNode(Node* n);
		void extendAtDef(const Node* n);
		void emitPhiCopies(I32 targetBlock, I32 predIdx);
		void emitTerminator(I32 b);
		void emitStore(StoreNode* s);
		void emitLoad(LoadNode* l);
		void emitBinary(BinaryNode* n);
		void emitAlu(BinaryNode* n, X86Op op);
		void emitDiv(BinaryNode* n, X86Op op);
		void emitShift(BinaryNode* n, X86Op op);
		void emitFloatBinary(BinaryNode* n);
		void emitVecBinary(BinaryNode* n);
		void emitVecShift(BinaryNode* n);
		void vshiftImm(VReg d, U32 laneBits, U32 idx, I64 cnt);
		void emitByteShift(BinaryNode* n, I64 cnt);
		void emitByteMul(BinaryNode* n);
		void emitSplat(SplatNode* n);
		void emitExtract(ExtractNode* n);
		void emitPack(PackNode* n);
		void packLaneZero(PackNode* n, VReg d, U32 esz, B32 isInt);
		void insertLane(VReg d, Node* lane, U32 i, U32 esz, B32 isInt);
		void repeatLowHalf(VReg d);
		void emitShuffle(ShuffleNode* n);
		void emitBitScan(UnaryNode* n, B32 reverse);
		void emitPopcnt(UnaryNode* n);
		void emitFNeg(UnaryNode* n);
		void emitGpSign(UnaryNode* n);
		void emitFSqrtAbs(UnaryNode* n);
		void emitUnary(UnaryNode* n);
		B32 holdsExt(const Node* n, Ext ext) const;
		B32 fitsWideCmp(const CompareNode* n) const;
		void emitWideCmp(CompareNode* n);
		void emitNarrowCmp(CompareNode* n);
		U8 emitFpCmp(CompareNode* n);
		U8 emitCmp(CompareNode* n);
		void emitSelect(SelectNode* n);
		VReg vecMask(CompareNode* c, B32& invert);
		VReg vecIntMask(Opcode op, VReg l, VReg r, U32 esz);
		B32 emitMinMax(SelectNode* n, CompareNode* c);
		void emitFMinMax(SelectNode* n, B32 max);
		void emitVecSelect(SelectNode* n);
		void emitCompare(CompareNode* n);
		void emitConvert(ConvertNode* n);
		void emitVecConvert(ConvertNode* n);
		void emitVecTrunc(ConvertNode* n);
		void emitIntResize(ConvertNode* n);
		void emitIntToFP(ConvertNode* n);
		void emitFPToInt(ConvertNode* n);
		Pair<VReg, VReg> splitHalves(VReg s);
		void emitU64ToFP(ConvertNode* n, VReg s, U32 w);
		void emitFPToU64(ConvertNode* n, MachineOperand x, MachineOperand k);
		void emitU64ToX87(ConvertNode* n, VReg s);
		void emitConvertX87(ConvertNode* n);

		// calls, params and returns
		B32 emitMathIntrinsic(CallNode* c);
		void emitAsm(AsmNode* a);
		B32 emitBuiltin(CallNode* c);
		void emitCall(CallNode* c);
		void pushCallArgs(CallNode* c, X86ArgAssigner& as, List<CallArg>& args);
		Ops callUses(CallNode* c, const List<CallArg>& args, U32 sseUsed);
		CallArg callArg(Node* arg, X86ArgAssigner& as);
		void callResult(Node* vp, const Type* rt, B32 sret, I32 retTemp);
		Ops moveRegReturn(VReg retSlot, const AbiRegs& regs, ReturnMove move);
		void moveX87Return(VReg retSlot, U32 count, ReturnMove move);
		VReg x87PartAddr(VReg base, U32 index);
		List<X86ArgAssigner::Loc> paramLocs(X86ArgAssigner& as);
		U32 emitPrologue();
		void maskBoolParams(const List<X86ArgAssigner::Loc>& locs);
		void lowerParam(U32 index, X86ArgAssigner::Loc l);
		void x87Param(ProjNode* p, X86ArgAssigner::Loc l);
		VReg stackParamAddr(U32 off);
		void emitReturn(ReturnNode* r);

		// debug binding placement
		void planBindings(BindingPlan& plan) const;
		U32 endIn(const Node* n, I32 b, const BindingPlan& plan) const;
		U32 sourcePos(const DebugBinding& bd, I32 b, const BindingPlan& plan) const;
		U32 valuePos(const DebugBinding& bd, I32 b, const BindingPlan& plan) const;
		MachineOperand bindingLoc(const Node* value) const;
		void placeBindings(I32 b, BindingPlan& plan);
	private:
		// target
		const X86CallConv* conv = &abi::kSysV;
		const RegisterInfo* regs = nullptr;
		U32 ptrBytes = 8;
		// current module, function and block
		Module* mod = nullptr;
		const Function* fn = nullptr;
		Schedule* sched = nullptr;
		MachineFunc* out = nullptr;
		X86FrameLayout* fl = nullptr;
		MachineBlock* mb = nullptr;
		U32 curLoc = 0;
		// per node id
		List<VReg> vregOf;
		List<Ext> extOf;
		List<I32> x87Slot; // node id -> 16-byte x87 frame slot, an rbp offset
	};
} // namespace rat

#endif
