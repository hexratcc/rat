#ifndef RAT_PASS_EMIT_X86EMITTER_H
#define RAT_PASS_EMIT_X86EMITTER_H

#include "Core.h"

#include "CodeGen/MachineFunction.h"
#include "CodeGen/Schedule.h"
#include "IR/Opcode.h"
#include "Support/Pass.h"
#include "Target/X86Asm.h"

namespace rat {
	struct AllocNode;
	struct BinaryNode;
	struct CallNode;
	struct CompareNode;
	struct ConvertNode;
	struct ElfObject;
	struct Function;
	struct Global;
	struct IfNode;
	struct LoadNode;
	struct MachineModule;
	struct Module;
	struct Node;
	struct ProjNode;
	struct PhiNode;
	struct ReturnNode;
	struct StoreNode;
	struct TargetInfo;
	struct Type;
	struct UnaryNode;

	namespace detail {
		constexpr U32 kGp = X86Target::kGpClass;
		constexpr U32 kFp = X86Target::kFpClass;
		constexpr U32 kX87 = X86Target::kX87Class;

		constexpr I64 kX87MemBits = 80;

		constexpr U8 kAluAdd = 0x01;
		constexpr U8 kAluSub = 0x29;
		constexpr U8 kAluAnd = 0x21;
		constexpr U8 kAluOr = 0x09;
		constexpr U8 kAluXor = 0x31;

		using namespace sysv;

		constexpr U8 kIntCc[] = {CC_E, CC_NE, CC_L, CC_LE, CC_B, CC_BE};

		constexpr U8 kSseOp[] = {0x58, 0x5c, 0x59, 0x5e};
	} // namespace detail

	enum class X86Op : U32 {
		// pseudo / data movement
		Copy,			 // dst = src (reg-reg or fill from a fixed phys; RA-coalescable)
		LoadImm,	 // dst = imm
		LoadSym,	 // dst = lea rip[sym]  (address of a global)
		LoadFrame, // dst = lea rbp[slot] (address of a frame slot, e.g. alloc storage)
		FrameAddr, // dst = lea rbp[imm]  (address of an arbitrary rbp offset; imm = disp)
		// integer memory: use[0] = address reg, imm = displacement
		Load,	 // dst = [addr + disp], sign/zero-extended per width/imm2
		Store, // [addr + disp] = src, width
		// integer ALU (two-address on the def reg; def and use[0] are coalesced)
		Add,
		Sub,
		Mul,
		And,
		Or,
		Xor,
		Neg,
		Not,
		Shl,
		AShr,
		LShr, // shift count in use[1] (fixed to RCX by lowering)
		SDiv,
		SRem,
		UDiv,
		URem,				 // RAX/RDX/RCX fixed by lowering via Phys operands
		Cmp,				 // flag-setting compare of use[0],use[1]
		SetCC,			 // dst = (cc); condition code in imm
		Movzx,			 // dst = zero-extend byte src
		MaskBits,		 // dst &= ((1<<imm)-1)
		SignExtBits, // dst = sign-extend dst from imm bits to 64
		// SSE scalar float
		FLoad,
		FStore, // [addr+disp] <-> xmm
		FAdd,
		FSub,
		FMul,
		FDiv, // two-address on the def xmm
		FNeg, // pxor-based scalar negate
		FCmp, // ucomis use[0],use[1]; cc in imm, swap in imm2
		Cvt,	// SSE convert; pfx/opc/w packed in imm
		// x87 ops
		X87LoadMem,	 // def(slot) = fld [use0 addr]; imm = mem width (4/8/80)
		X87StoreMem, // [use0 addr] = fstp use1(slot); imm = mem width (4/8/80)
		X87LoadImmD, // def(slot) = fld double-bits in use0.imm (via scratch slot)
		X87FromInt,	 // def(slot) = fild use0 (gp), through scratch slot
		X87ToInt,		 // def(gp)   = fistp use0(slot), through scratch slot + cw juggle
		X87FromSse,	 // def(slot) = (long double)use0 (xmm); imm = src width
		X87ToSse,		 // def(xmm)  = (float)use0(slot);   imm = dst width
		X87Add,
		X87Sub,
		X87Mul,
		X87Div, // def(slot) = use0(slot) OP use1(slot)
		X87Neg, // def(slot) = -use0(slot) (fchs)
		X87Cmp, // def(gp) = use0(slot) CMP use1(slot); cc in imm, swap in imm2
		// control / calls
		Call, // direct (sym in sym) or indirect (target reg in use[0]); imm = xmmUsed
		Ret,
		Jmp, // unconditional, target block in use[0].block
		Br,	 // test+jcc: predicate in use[0], then in use[1].block, else in use[2].block
		// variadic support
		VaStart, // init va_list at [use0]; imm = gpOffset start, imm2 = fpOffset start
		VaArg,	 // fetch next vararg into def; use0 = va_list ptr; imm = kind, imm2 = width
	};

	enum class VaArgKind : I64 { Int = 0, Sse = 1, X87 = 2 };

	struct X86FrameLayout : MachineFuncAux {
		I32 ldScratch = 0;
		B32 variadic = false;
		I32 saveArea = 0;
		I32 overflowOff = 16;
		U32 namedGp = 0;
		U32 namedFp = 0;
	};

	RegAllocHooks x86RegAllocHooks();

	struct X86LowerPass : MachinePass {
		const C8* name() const override { return "x86-lower"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		U32 runOnMachineFunction(const Function& fn, MachineFunc& mf, const TargetInfo& target);

		void reset(const Function& f, Schedule& s, MachineFunc& o, X86FrameLayout& layout);
		void lowerFunction();

		static PhysReg gpReg(Reg r);
		static PhysReg xmmReg(U32 n);
		static B32 isFloatTy(const Type* t);
		static B32 isX87Ty(const Type* t);
		static B32 isSseTy(const Type* t);
		static U32 intBits(const Type* t);
		static U32 opWidth(const Type* t);
		static String libcName(const String& callee);

		I32 reserve(U32 bytes);
		void layout();
		void layoutVariadic();
		U32 classOf(const Type* t) const;
		VReg fresh(U32 cls);
		I32 x87SlotOf(const Node* n);
		VReg vregFor(const Node* n);
		void emit(MachineInstr in);
		MachineInstr& inst(X86Op op,
											 U32 cls,
											 List<MachineOperand> defs,
											 List<MachineOperand> uses,
											 I64 imm = 0,
											 I64 imm2 = 0);
		void copy(MachineOperand dst, MachineOperand src, U32 cls);
		MachineInstr& def1(X86Op op, VReg dst, U32 cls, List<MachineOperand> uses);
		VReg gpValue(Node* n);
		VReg sseValue(Node* n);
		I32 x87Value(Node* n);
		void storeIntTo(VReg addr, VReg src, U32 w);
		void emitStore(StoreNode* s);
		void emitLoad(LoadNode* l);
		void emitAlloc(AllocNode* al);
		void twoAddr(X86Op op, VReg d, VReg lhs, VReg rhs);
		void maskBits(VReg d, U32 bits);
		void signExtBits(VReg d, U32 bits);
		void emitDivLike(BinaryNode* n, X86Op op);
		void emitShift(BinaryNode* n, X86Op op);
		void emitBinary(BinaryNode* n);
		void emitFloatBinary(BinaryNode* n);
		void emitX87Binary(BinaryNode* n, U32 idx);
		void emitUnary(UnaryNode* n);
		void emitCompare(CompareNode* n);
		void emitFloatCompare(CompareNode* n);
		static I64 cvtDesc(U8 pfx, U8 opc, B32 w);
		void emitConvert(ConvertNode* n);
		void emitConvertX87(ConvertNode* n, Node* src, Opcode op);
		List<PhysReg> callerSavedClobbers() const;
		void emitCall(CallNode* c);
		void emitPrologue();
		void loadStackParam(ProjNode* p, Type* t, I32 disp);
		void emitVaStart(CallNode* c);
		void emitVaArg(CallNode* c);
		void emitNode(Node* n);
		void emitReturn(ReturnNode* r);
		void emitPhiCopies(I32 targetBlock, I32 predIdx);
		void emitTerminator(I32 b);
	private:
		const Function* fn = nullptr;
		const Module* mod = nullptr;
		Schedule* sched = nullptr;
		MachineFunc* out = nullptr;
		X86FrameLayout* fl = nullptr;
		Map<const Node*, VReg> vregOf;
		Map<const Node*, I32> x87Slot;
		Map<const Node*, I32> allocOff;
		MachineBlock* mb = nullptr;
	};

	struct X86EncodePass : MachinePass {
		explicit X86EncodePass(std::ostream& os)
		: os(&os) {}

		const C8* name() const override { return "x86-encode"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	private:
		struct JumpFix {
			U32 dispAt;			 // offset of the rel32 displacement in code
			I32 targetBlock; // block the jump targets
		};

		void emitGlobal(ElfObject& elf, const Module& mod, const Global* g);

		void reset(const MachineFunc& f, const X86FrameLayout& layout, Asm& asm_, List<PhysReg> callee);
		void encodeFunction();

		static Reg toGp(PhysReg p);
		static U32 toXmm(PhysReg p);
		static Reg gpOf(const MachineOperand& o);
		static U32 xmmOf(const MachineOperand& o);
		static PhysReg gpReg11();
		void readGp(const MachineOperand& o, Reg r);
		void emitCopy(const MachineInstr& in);
		void emitLoadImm(const MachineInstr& in);
		void emitLoadSym(const MachineInstr& in);
		void emitFrameAddr(const MachineInstr& in);
		void emitLoadFrame(const MachineInstr& in);
		void emitLoad(const MachineInstr& in);
		void emitStore(const MachineInstr& in);
		void emitFLoad(const MachineInstr& in);
		void emitFStore(const MachineInstr& in);
		void emitAlu(const MachineInstr& in, U8 aluOp);
		void emitMul(const MachineInstr& in);
		void emitNegNot(const MachineInstr& in, B32 neg);
		void emitShift(const MachineInstr& in, U8 ext);
		void emitDiv(const MachineInstr& in, B32 isSigned);
		void emitMaskBits(const MachineInstr& in);
		void emitSignExtBits(const MachineInstr& in);
		void emitCmp(const MachineInstr& in);
		void emitSetCC(const MachineInstr& in);
		void emitFArith(const MachineInstr& in, U8 op);
		void emitFNeg(const MachineInstr& in);
		void emitFCmp(const MachineInstr& in);
		void emitCvt(const MachineInstr& in);
		void fldSlot(I32 slot);
		void fstpSlot(I32 slot);
		void emitX87LoadMem(const MachineInstr& in);
		void emitX87StoreMem(const MachineInstr& in);
		void emitX87LoadImmD(const MachineInstr& in);
		void emitX87FromInt(const MachineInstr& in);
		void emitX87ToInt(const MachineInstr& in);
		void emitX87FromSse(const MachineInstr& in);
		void emitX87ToSse(const MachineInstr& in);
		void emitX87Binary(const MachineInstr& in, U32 idx);
		void emitX87Neg(const MachineInstr& in);
		void emitX87Cmp(const MachineInstr& in);
		void emitVaStart(const MachineInstr& in);
		void vaFetchOverflow(I32 step);
		void vaFetch(I32 offDisp, U32 limit, I32 regStep);
		void emitVaArg(const MachineInstr& in);
		void emitCall(const MachineInstr& in);
		void recordFix(U32 dispAt, I32 targetBlock);
		void emitRet(const MachineInstr&);
		void emitJmp(const MachineInstr& in, I32 fallthrough);
		void emitBr(const MachineInstr& in, I32 fallthrough);
		void emitInst(const MachineInstr& in, I32 fallthrough);
		void prologue();
	private:
		std::ostream* os;
		const MachineFunc* fn = nullptr;
		const X86FrameLayout* fl = nullptr;
		Asm* a = nullptr;
		List<U32> blockOffset; // block id -> byte offset in code
		List<JumpFix> fixes;
		U32 frameSize = 0;
		List<PhysReg> calleeSaved; // callee-saved GP regs the allocator used
		I32 calleeBase = 0;				 // RBP offset of the first callee-save slot
	};
} // namespace rat

#endif
