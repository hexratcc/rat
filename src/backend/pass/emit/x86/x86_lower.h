#ifndef RAT_PASS_EMIT_X86LOWER_H
#define RAT_PASS_EMIT_X86LOWER_H

#include "core.h"

#include "codegen/schedule.h"
#include "ir/debug_info.h"
#include "ir/opcode.h"
#include "pass/emit/x86/x86_op.h"
#include "pass/pass.h"

namespace rat {
	struct BinaryNode;
	struct CallNode;
	struct AsmNode;
	struct CompareNode;
	struct ConstantNode;
	struct ConvertNode;
	struct Function;
	struct ExtractNode;
	struct LoadNode;
	struct Node;
	struct PackNode;
	struct ProjNode;
	struct SelectNode;
	struct ShuffleNode;
	struct ReturnNode;
	struct SplatNode;
	struct StoreNode;
	struct Type;
	struct UnaryNode;

	namespace detail {
		void x86Reload(MachineInstr& m, PhysReg dst, I32 slot, U32 cls, U32 width);
		void x86Spill(MachineInstr& m, I32 slot, PhysReg src, U32 cls, U32 width);
		I32 x86AllocSlot(MachineFunc& fn, U32 cls, U32 width);
		B32 x86IsCopy(const MachineInstr& in);
	} // namespace detail

	struct X86LowerPass : MachinePass {
		const C8* name() const override { return "x86-lower"; }
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
	private:
		// matched address, after its base and index are in registers
		struct AddrParts {
			VReg base = 0;
			VReg index = 0;
			U32 scaleLog2 = 0;
			I32 disp = 0;
			B32 hasIndex = false;
			B32 frameBase = false; // rbp-rel
		};
		struct AddrMatch {
			Node* base = nullptr;
			Node* index = nullptr;
			Node* scaleNode = nullptr;
			U32 scaleLog2 = 0;
			I32 disp = 0;
			B32 hasIndex = false;
		};
		using Kind = X86ArgAssigner::Kind;
		using Ops = MachineOperands;
		using OpList = std::initializer_list<MachineOperand>;
		struct CallArg {
			MachineOperand val; // vreg, or a 16-byte frame slot for by-value x87
			Kind cls;
			I32 reg;
		};
		enum class ReturnMove : U8 {
			IntoSlot,
			OutOfSlot
		};

		void lowerFn(const Function& f, MachineFunc& mf, const TargetInfo& target);
		void lowerBlocks();

		// operand shorthands
		static MachineOperand vr(VReg v, U32 w = 8);
		static MachineOperand ph(Reg r);
		static MachineOperand xm(Xmm x);
		static MachineOperand im(Imm i);
		static MachineOperand sl(Slot s);
		static MachineOperand blockRef(I32 b);
		static B32 isX87Ty(const Type* t);
		static B32 isSseTy(const Type* t);
		static U32 intBits(const Type* t);
		static B32 immOf(Node* n, I64& out);
		static B32 branchOnlyCompare(Node* n);
		static B32 selectOnlyCompare(Node* n);
		static B32 fusableCompare(Node* n);
		static U32 log2Scale(I64 c);
		static BinaryNode* asAdd(Node* n);
		static B32 onlyUsedBy(const Node* n, Opcode op);
		static B32 zextOnlyLoad(const LoadNode* l);
		static B32 lowHalfOnlyLoad(const LoadNode* l);
		static B32 splatOnlyLoad(const Node* n);
		static BinaryNode* addSubOf(const ShuffleNode* n);
		static B32 addSubOnly(const Node* n);
		static U32 opWidth(const Type* t);

		I32 reserve(U32 bytes, U32 align = 8);
		void needScratch();
		void layout();
		U32 classOf(const Type* t) const;
		Kind argKind(const Type* t) const;
		VReg fresh(U32 cls);
		Slot x87SlotOf(const Node* n);
		VReg vregFor(const Node* n);
		MachineInstr& put(X86Op op, OpList defs, Ops uses, I64 imm = 0, I64 imm2 = 0);

		// data movement
		void mov(VReg d, VReg s);
		void mov(Reg d, VReg s);
		void mov(VReg d, Reg s);
		void mov(Reg d, Reg s);
		void mov(MachineOperand d, MachineOperand s, U32 cls);
		void movaps(VReg d, VReg s, U32 w);
		void movaps(VReg d, Xmm s);
		void movaps(Xmm d, VReg s);
		void lea(VReg d, const String& s);
		void lea(VReg d, const AddrParts& a);
		// integer memory
		void ld(VReg d, VReg base);
		void ld(VReg d, Slot s);
		void ld(VReg d, U32 w, const AddrParts& a, B32 sign);
		void ld(VReg d, U32 w, VReg base, B32 sign);
		void st(VReg base, MachineOperand src);
		void st(Slot d, Reg src);
		void st(const AddrParts& a, MachineOperand src);
		// integer ALU
		void alu(X86Op op, VReg d, VReg a, VReg b); // Add..Xor
		void alu(X86Op op, VReg d, VReg a, Imm b);
		void shift(X86Op op, VReg d, Imm cnt); // Shl / AShr / LShr
		void shift(X86Op op, VReg d, Reg cl);
		void cmp(VReg a, VReg b);
		void cmp(VReg a, Imm b);
		// sse scalar float
		void ldf(VReg d, U32 w, VReg base);
		void ldf(VReg d, U32 w, const String& s);
		void ldf(VReg d, U32 w, const AddrParts& a);
		MachineInstr& farith(X86Op op, VReg d, VReg a, VReg b, U32 w, I64 desc);
		void cvtf(VReg d, U32 dw, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide); // Cvt, xmm dest
		// sse packed vector
		static I64 vecDesc(U8 pfx, U8 opc, U8 esc);
		void varith(VReg d, VReg a, VReg b, U8 pfx, U8 opc, B32 esc38);
		void varithImm(VReg d, VReg a, VReg b, U8 pfx, U8 opc, U8 esc, U8 imm8);
		void vmerge(VReg d, VReg s, U8 pfx, U8 opc, U8 esc, I32 imm8);
		void vunary(VReg d, VReg s, U8 pfx, U8 opc, U8 esc);
		void vblend(VReg d, VReg f, VReg t, VReg mask, U32 esz);
		// x87
		void fldSlot(Slot d, Slot s);

		VReg gpValue(Node* n);

		B32 scaleOf(Node* n, Node*& idx, U32& scaleLog2);
		AddrMatch decodeAddr(Node* ptr);
		AddrParts matchAddr(Node* ptr);
		B32 addressOnlyAdd(Node* n);
		B32 addressOnlyScale(Node* n);
		MachineOperand addrBase(const AddrParts& a);
		I64 sibBits(I64 sign, const AddrParts& a);
		Ops addrUses(const AddrParts& a);
		VReg sseValue(Node* n);
		const String& fpPoolSym(U64 bits, U32 width);
		void fpConstLoad(ConstantNode* c, VReg dst);
		Slot x87Value(Node* n);
		void emitStore(StoreNode* s);
		void emitLoad(LoadNode* l);
		VReg frameAddr(I64 disp);
		static AddrParts baseDisp(VReg base, I32 disp);
		void maskBits(VReg d, U32 bits);
		void signExtBits(VReg d, U32 bits);
		void emitDivLike(BinaryNode* n, X86Op op);
		void emitShift(BinaryNode* n, X86Op op);
		void emitRotate(BinaryNode* n, X86Op op);
		void emitAlu(BinaryNode* n, X86Op op);
		void mulImm(VReg d, VReg s, I64 v);
		void emitBinary(BinaryNode* n);
		void emitFloatBinary(BinaryNode* n);
		void emitVecBinary(BinaryNode* n);
		void emitVecShift(BinaryNode* n);
		void emitSplat(SplatNode* n);
		void emitExtract(ExtractNode* n);
		List<U8> constLanes(PackNode* n, U32 esz);
		B32 emitConstPack(PackNode* n, U32 esz);
		void emitPack(PackNode* n);
		void emitPackMem(PackNode* n, U32 esz, B32 isInt);
		void packLaneZero(PackNode* n, VReg d, U32 esz, B32 isInt);
		void insertLane(VReg d, Node* lane, U32 i, U32 esz, B32 isInt);
		void emitAddSub(ShuffleNode* n, BinaryNode* sub);
		static void shuffleDwords(const ShuffleNode* n, U8& sel, U8& mask, B32& inPlace);
		void emitShuffle(ShuffleNode* n);
		void emitSelect(SelectNode* n);
		VReg vecMask(CompareNode* c, B32& invert);
		VReg vecIntMask(Opcode op, VReg l, VReg r, U32 esz);
		B32 emitVecMinMax(SelectNode* n, CompareNode* c);
		void emitVecSelect(SelectNode* n);
		const String& vecPoolSym(const List<U8>& bytes);
		VReg gpConst(I64 v);
		void emitBitScan(UnaryNode* n, B32 reverse);
		void emitPopcnt(UnaryNode* n);
		void emitFNeg(UnaryNode* n);
		void emitUnary(UnaryNode* n);
		U8 emitCmp(CompareNode* n);
		void emitCompare(CompareNode* n);
		static I64 cvtDesc(U8 pfx, U8 opc, B32 w);
		void emitConvert(ConvertNode* n);
		void emitVecConvert(ConvertNode* n);
		static U8 pmovOpc(U32 sw, U32 dw, B32 zext);
		void emitIntResize(ConvertNode* n, Node* src);
		void emitIntToFP(ConvertNode* n, Node* src);
		void emitFPToInt(ConvertNode* n, Node* src);
		Pair<VReg, VReg> splitHalves(VReg s);
		void blendU64(ConvertNode* n, VReg lo, VReg hi, VReg m);
		void emitU64ToFP(ConvertNode* n, VReg s, U32 w);
		void emitFPToU64(ConvertNode* n, Node* src);
		void emitUIntToX87(ConvertNode* n, VReg s, U32 bits);
		void emitX87ToU64(ConvertNode* n, Slot x);
		void emitConvertX87(ConvertNode* n, Node* src, Opcode op);
		U64 callerSavedClobbers() const;
		U64 allRegClobbers() const;
		B32 emitBuiltin(CallNode* c);
		void emitCall(CallNode* c);
		Ops callUses(CallNode* c, const List<CallArg>& args, U32 sseUsed);
		void pushCallArgs(CallNode* c, X86ArgAssigner& as, List<CallArg>& args);
		CallArg callArg(Node* arg, X86ArgAssigner& as);
		MachineOperand argToReg(const CallArg& a);
		void callResult(Node* vp, const Type* rt, B32 sret, I32 retTemp);
		static MachineOperand returnReg(AbiClass c, U32& gp, U32& sse);
		Ops moveRegReturn(VReg retSlot, const AbiRegs& regs, ReturnMove move);
		void storeX87Return(VReg retSlot, U32 count);
		void loadX87Return(VReg retSlot, U32 count);
		VReg x87PartAddr(VReg base, U32 index);
		void emitAsm(AsmNode* a);
		B32 emitMathIntrinsic(CallNode* c);
		void emitPrologue();
		List<X86ArgAssigner::Loc> paramLocs(X86ArgAssigner& as);
		void lowerParam(U32 index, X86ArgAssigner::Loc l);
		void x87Param(ProjNode* p, X86ArgAssigner::Loc l);
		VReg stackParamAddr(U32 off);
		void emitVaArg(CallNode* c);
		void vaArgAggregate(CallNode* c);
		void emitNode(Node* n);
		void emitReturn(ReturnNode* r);
		void moveValue(VReg dst, VReg src, U32 cls, U32 w);
		void emitPhiCopies(I32 targetBlock, I32 predIdx);
		void emitTerminator(I32 b);
		// debug bindings of the function, grouped by the block of their anchor
		struct BindingPlan {
			List<U32> first;	 // block -> first of its bindings in order
			List<U32> order;	 // binding indexes
			List<U32> nodeEnd; // node id -> insts index past its code
			List<U32> cursor;	 // var inst -> least position left in the block
			U32 prologueEnd = 0;
		};
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
		B32 sse41 = true;
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
		List<I32> x87Slot;
		List<I32> allocOff;
		List<Pair<PhiNode*, VReg>> phiMoves;
		List<Pair<PhiNode*, Slot>> x87Moves;
	};
} // namespace rat

#endif
