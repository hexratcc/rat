#ifndef RAT_PASS_EMIT_X86ENCODE_H
#define RAT_PASS_EMIT_X86ENCODE_H

#include "core.h"

#include "pass/emit/x86/x86_op.h"
#include "pass/pass.h"
#include "target/x86/x86_asm.h"

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

		struct PendingTable {
			U32 leaDispAt;		 // rip-relative disp of the lea that loads the table base
			List<I32> targets; // slot target blocks
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
		void emitFrameAddr(const MachineInstr& in);
		void emitStackAlloc(const MachineInstr& in);
		void emitStackSave(const MachineInstr& in);
		void emitStackRestore(const MachineInstr& in);
		void emitSetJmp(const MachineInstr& in);
		void emitLongJmp(const MachineInstr& in);
		void emitLoad(const MachineInstr& in);
		void emitStore(const MachineInstr& in);
		void emitFLoad(const MachineInstr& in);
		void emitFStore(const MachineInstr& in);
		void emitAlu(const MachineInstr& in, U8 aluOp);
		void emitShift(const MachineInstr& in, U8 ext);
		void emitDiv(const MachineInstr& in, B32 isSigned);
		void emitMaskBits(const MachineInstr& in);
		void emitSignExtBits(const MachineInstr& in);
		void emitCmp(const MachineInstr& in);
		void setccExt(U8 cc, Reg d);
		void emitFNeg(const MachineInstr& in);
		void emitFAbs(const MachineInstr& in);
		void emitFCmp(const MachineInstr& in);
		void emitFCmpFlags(const MachineInstr& in);
		void emitCvt(const MachineInstr& in);
		void emitVArith(const MachineInstr& in);
		void emitVSplat(const MachineInstr& in);
		void emitVExtract(const MachineInstr& in);
		void emitVPack(const MachineInstr& in);
		void emitVPackReg(const MachineInstr& in);
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
		void vaPtrToR10(const MachineInstr& in);
		void vaLoadResult(const MachineInstr& in);
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
		void emitSwitchJump(const MachineInstr& in);
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
		List<PendingTable> tables;
		U32 frameSize = 0;
		B32 omitFrame = false;
		B32 hasDynAlloca = false;
		List<PhysReg> calleeSaved; // callee-saved GP regs the allocator used
	};
} // namespace rat

#endif
