#ifndef RAT_PASS_EMIT_X86LOWER_H
#define RAT_PASS_EMIT_X86LOWER_H

#include "core.h"

#include "codegen/schedule.h"
#include "ir/opcode.h"
#include "pass/emit/x86/x86_op.h"
#include "pass/pass.h"

#include <cstdint>

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

	struct X86LowerPass : MachinePass {
		const C8* name() const override { return "x86-lower"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
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

		void runOnMachineFunction(const Function& fn, MachineFunc& mf, const TargetInfo& target);

		void reset(const Function& f, Schedule& s, MachineFunc& o, X86FrameLayout& layout);
		void lowerFunction();

		static PhysReg gpReg(Reg r);
		static PhysReg xmmReg(U32 n);
		static B32 isFloatTy(const Type* t);
		static B32 isX87Ty(const Type* t);
		static B32 isSseTy(const Type* t);
		static U32 intBits(const Type* t);
		static B32 isIntCompare(Node* n);
		static B32 immOf(Node* n, I64& out);
		static B32 branchOnlyCompare(Node* n);
		static B32 onlySelectCondUsers(Node* n);
		static B32 selectOnlyCompare(Node* n);
		static B32 fpSelectOnlyCompare(Node* n);
		static B32 fusableFpCompare(Node* n);
		static B32 zextOnlyLoad(const LoadNode* l);
		static U32 opWidth(const Type* t);

		I32 reserve(U32 bytes, U32 align = 8);
		void needScratch();
		void layout();
		void layoutVariadic();
		U32 classOf(const Type* t) const;
		VReg fresh(U32 cls);
		I32 x87SlotOf(const Node* n);
		VReg vregFor(const Node* n);
		void emit(MachineInstr in);
		MachineInstr&
		put(X86Op op, List<MachineOperand> defs, List<MachineOperand> uses, I64 imm = 0, I64 imm2 = 0);

		// data movement
		void mov(VReg d, VReg s);
		void mov(Reg d, VReg s);
		void mov(VReg d, Reg s);
		void mov(Reg d, Reg s);
		void mov(MachineOperand d, MachineOperand s, U32 cls);
		void movaps(VReg d, VReg s, U32 w);
		void movaps(VReg d, Xmm s);
		void movaps(Xmm d, VReg s);
		void movi(VReg d, I64 v);
		void lea(VReg d, const String& s);
		void lea(VReg d, const AddrParts& a);
		void leaFrame(VReg d, I64 disp);
		void retAddr(VReg d);
		// integer memory
		void ld(VReg d, VReg base);
		void ld(VReg d, Slot s);
		void ld(VReg d, U32 w, const AddrParts& a, B32 sign);
		void ld(VReg d, U32 w, VReg base, B32 sign);
		void st(VReg base, MachineOperand src);
		void st(Slot d, Reg src);
		void st(const AddrParts& a, MachineOperand src);
		// dynamic stack
		void stackAlloc(VReg d, VReg size);
		void stackSave(VReg d);
		void stackRestore(VReg sp);
		// non-local goto
		void setJmp();
		void longJmp();
		// integer ALU
		void alu(X86Op op, VReg d, VReg a, VReg b); // Add..Xor
		void alu(X86Op op, VReg d, VReg a, Imm b);
		void imul(VReg d, VReg a, Imm b);
		void neg(VReg d);
		void not_(VReg d);
		void shift(X86Op op, VReg d, Imm cnt); // Shl / AShr / LShr
		void shift(X86Op op, VReg d, Reg cl);
		void rot(X86Op op, VReg d, I64 cnt, U32 bits); // Rotl / Rotr
		void idiv(X86Op op, U32 bits);
		void bitScan(X86Op op, VReg d, VReg s, U32 w);
		void cmp(VReg a, VReg b);
		void cmp(VReg a, Imm b);
		void setcc(VReg d, U8 cc);
		void cmov(VReg d, VReg s, U8 cc);
		void maskBitsOp(VReg d, U32 bits);
		void signExtBitsOp(VReg d, U32 bits);
		void bswap(VReg d, U32 w);
		// sse scalar float
		void ldf(VReg d, U32 w, VReg base);
		void ldf(VReg d, U32 w, const String& s);
		void ldf(VReg d, U32 w, const AddrParts& a);
		void stf(const AddrParts& a, VReg s, U32 w);
		void farith(X86Op op, VReg d, VReg a, VReg b, U32 w, I64 desc);
		void fneg(VReg d, VReg s, U32 w);
		void fsqrt(VReg d, VReg s, U32 w);
		void fabs_(VReg d, VReg s, U32 w);
		void ucomis(VReg d, VReg a, VReg b, U32 w, U8 cc, B32 swap);				 // FCmp
		void ucomisFlags(VReg a, VReg b, U32 w, B32 swap);									 // FCmpFlags
		void cvtf(VReg d, U32 dw, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide); // Cvt, xmm dest
		void cvti(VReg d, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide);				 // Cvt, gp dest
		// sse packed vector
		void varith(VReg d, VReg a, VReg b, U8 pfx, U8 opc, B32 esc38);
		void vsplat(VReg d, VReg s, U32 esz, B32 isInt);
		void vextract(VReg d, VReg s, U32 lane, U32 esz, B32 isInt);
		void vpack(X86Op op, VReg d, List<MachineOperand> lanes, U32 esz, B32 isInt);
		void vshuf(VReg d, VReg s, U8 sel);
		// x87
		void fld(Slot d, VReg addr);
		void fldPop(Slot s);
		void fstp(VReg addr, Slot s);
		void fstp(Slot d);
		void fstpDiscard();
		void fldImm(Slot d, U64 bits);
		void fild(Slot d, VReg s);
		void fistp(VReg d, Slot s);
		void fldSse(Slot d, VReg s, U32 w);
		void fldSlot(Slot d, Slot s);
		void fstpSse(VReg d, U32 w, Slot s);
		void x87Arith(X86Op op, Slot d, Slot a, Slot b);
		void fchs(Slot d, Slot s);
		void fucomi(VReg d, Slot a, Slot b, U8 cc, B32 swap);
		// control
		void jmp(I32 target);
		void jcc(U8 cc, I32 thenB, I32 elseB);
		void br(VReg pred, I32 thenB, I32 elseB);
		void switchJump(VReg sel, const List<I32>& targets);
		void vaStart(VReg ptr, U32 namedGp, U32 namedFp);
		void vaArg(MachineOperand def, VReg ptr, VaArgKind kind, I64 desc, U32 cls);
		void ud2();
		void prefetch(VReg addr, U8 hint);

		VReg gpValue(Node* n);

		struct AddrMatch {
			Node* base = nullptr;
			Node* index = nullptr;
			Node* scaleNode = nullptr;
			U32 scaleLog2 = 0;
			I32 disp = 0;
			B32 hasIndex = false;
		};
		B32 scaleOf(Node* n, Node*& idx, U32& scaleLog2);
		AddrMatch decodeAddr(Node* ptr);
		AddrParts matchAddr(Node* ptr);
		B32 addressOnlyAdd(Node* n);
		B32 addressOnlyScale(Node* n);
		MachineOperand addrBase(const AddrParts& a);
		I64 sibBits(I64 sign, const AddrParts& a);
		List<MachineOperand> addrUses(const AddrParts& a);
		VReg sseValue(Node* n);
		String fpPoolSym(U64 bits, U32 width);
		void fpConstLoad(ConstantNode* c, VReg dst);
		VReg fpConst(U64 bits, U32 width);
		I32 x87Value(Node* n);
		void x87Move(I32 dst, I32 src);
		void emitStore(StoreNode* s);
		void emitLoad(LoadNode* l);
		void emitStackAlloc(Node* n);
		void emitStackSave(Node* n);
		void emitStackRestore(Node* n);
		void twoAddr(X86Op op, VReg d, VReg lhs, VReg rhs);
		VReg frameAddr(I64 disp);
		void maskBits(VReg d, U32 bits);
		void signExtBits(VReg d, U32 bits);
		void emitDivLike(BinaryNode* n, X86Op op);
		void emitShift(BinaryNode* n, X86Op op);
		void emitRotate(BinaryNode* n, B32 left);
		void emitBinary(BinaryNode* n);
		void emitFloatBinary(BinaryNode* n);
		void emitVecBinary(BinaryNode* n);
		void emitSplat(SplatNode* n);
		void emitExtract(ExtractNode* n);
		void emitPack(PackNode* n);
		void emitShuffle(ShuffleNode* n);
		void emitSelect(SelectNode* n);
		void needVecScratch();
		String vecPoolSym(const List<U8>& bytes);
		void emitX87Binary(BinaryNode* n, U32 idx);
		void gpAcc(X86Op op, VReg d, VReg s);
		void gpShrImm(VReg d, U32 cnt);
		VReg gpConst(I64 v);
		void emitBitScan(UnaryNode* n, B32 reverse);
		void emitPopcnt(UnaryNode* n);
		void emitBswap(UnaryNode* n);
		B32 emitInlineIntrinsic(CallNode* n);
		void emitUnary(UnaryNode* n);
		U8 emitIntCmp(CompareNode* n);
		U8 fusedFpCmp(CompareNode* n);
		void emitCompare(CompareNode* n);
		void emitFloatCompare(CompareNode* n);
		void unorderedFixup(Opcode op, VReg d);
		static I64 cvtDesc(U8 pfx, U8 opc, B32 w);
		void emitConvert(ConvertNode* n);
		void emitU64ToFP(ConvertNode* n, VReg s, U32 w);
		void emitFPToU64(ConvertNode* n, Node* src);
		void emitConvertX87(ConvertNode* n, Node* src, Opcode op);
		List<PhysReg> callerSavedClobbers() const;
		List<PhysReg> allRegClobbers() const;
		void emitCall(CallNode* c);
		void emitAsm(AsmNode* a);
		B32 emitMathIntrinsic(CallNode* c);
		VReg x87ByRefArg(Node* arg);
		void emitPrologue();
		void loadStackParam(ProjNode* p, Type* t, I32 disp);
		void emitVaStart(CallNode* c);
		void emitVaArg(CallNode* c);
		void emitSetJmp(CallNode* c);
		void emitLongJmp(CallNode* c);
		void emitNode(Node* n);
		void emitReturn(ReturnNode* r);
		void phiMove(VReg dst, VReg src, U32 cls, U32 w);
		void emitPhiCopies(I32 targetBlock, I32 predIdx);
		void emitTerminator(I32 b);
	private:
		const Function* fn = nullptr;
		const X86CallConv* conv = &abi::kSysV;
		const RegisterInfo* regs = nullptr;
		U32 ptrBytes = 8;
		B32 sse41 = true;
		Schedule* sched = nullptr;
		MachineFunc* out = nullptr;
		X86FrameLayout* fl = nullptr;
		static constexpr I32 kNoSlot = INT32_MIN;
		List<VReg> vregOf;
		Module* mod = nullptr;
		List<I32> x87Slot;
		List<I32> allocOff;
		MachineBlock* mb = nullptr;
	};
} // namespace rat

#endif
