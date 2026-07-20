#ifndef RAT_PASS_EMIT_X86ENCODE_H
#define RAT_PASS_EMIT_X86ENCODE_H

#include "Core.h"

#include "Pass/Emit/X86Op.h"
#include "Pass/Pass.h"
#include "Target/X86Asm.h"

namespace rat {
	struct Global;
	struct ObjectFile;

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

		void emitGlobal(ObjectFile& obj, const Global* g, U32 ptrBytes);

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
		void emitLea(const MachineInstr& in);
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
		void emitVaStartWin64(const MachineInstr& in);
		void emitVaArgWin64(const MachineInstr& in);
		void emitCall(const MachineInstr& in);
		void recordFix(U32 dispAt, I32 targetBlock);
		void emitRet(const MachineInstr&);
		void emitJmp(const MachineInstr& in, I32 fallthrough);
		void emitBr(const MachineInstr& in, I32 fallthrough);
		void emitInst(const MachineInstr& in, I32 fallthrough);
		void prologue();
	private:
		std::ostream* os;
		const X86CallConv* conv = &abi::kSysV;
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
