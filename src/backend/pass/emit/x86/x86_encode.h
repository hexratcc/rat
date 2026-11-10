#ifndef RAT_PASS_EMIT_X86ENCODE_H
#define RAT_PASS_EMIT_X86ENCODE_H

#include "core.h"

#include "pass/emit/x86/x86_op.h"
#include "pass/pass.h"
#include "target/dwarf.h"
#include "target/x86/x86_asm.h"

#include <iosfwd>

namespace rat {
	struct Global;
	struct ObjectFile;

	struct X86EncodePass : MachinePass {
		explicit X86EncodePass(std::ostream& os)
		: os(&os) {}

		const C8* name() const override { return "x86-encode"; }
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
		void finish(Module& module, const TargetInfo& target) override;
	private:
		struct JumpFix {
			U32 dispAt;			 // offset of the rel32 displacement in code
			I32 targetBlock; // block the jump targets
		};

		struct PendingTable {
			U32 leaDispAt;		 // rip-relative disp of the lea that loads the table base
			List<I32> targets; // slot target blocks
		};

		struct PlacedFunc {
			const Function* fn;
			U32 offset;		// in text
			U32 relocEnd; // one past its last reloc
		};

		// a debug var location not yet closed into a VarRange, lists of them are sorted by inst
		struct OpenVarRange {
			U32 inst;
			VarLoc loc; // reg is a PhysReg
			U32 start;
		};
		static constexpr U32 kNoPc = ~0u;									// solving, no ranges
		static constexpr U64 kMaxVarTrackCost = 20000000; // instrs times tracked vars

		static void emitGlobal(ObjectFile& obj, const Global* g, U32 ptrBytes);
		void defineFunctions();

		static U32 blockIdBound(const MachineFunc& f);
		static B32 touchesFrame(const MachineOperand& o);
		static B32 needsFrame(const MachineInstr& in);
		void encodeFunction(const MachineFunc& f, Asm& asm_);

		static Reg toGp(PhysReg p);
		static Reg gpOf(const MachineOperand& o);
		static U32 xmmOf(const MachineOperand& o);
		static U32 scaleOf(const MachineInstr& in);
		static U8 laneSel(U32 lane, U32 esz);
		void readGp(const MachineOperand& o, Reg r);
		void copyXmm(U32 d, U32 s);
		void stash(Reg r);
		void unstash(Reg r);
		void immToScratch(I64 v);
		void emitCopy(const MachineInstr& in);
		void emitLoadImm(const MachineInstr& in);
		void emitStackAlloc(const MachineInstr& in);
		void emitSetJmp(const MachineInstr& in);
		void emitLongJmp(const MachineInstr& in);
		void emitLoad(const MachineInstr& in);
		void emitStore(const MachineInstr& in);
		void emitFLoad(const MachineInstr& in);
		void emitFStore(const MachineInstr& in);
		void emitAlu(const MachineInstr& in, U8 aluOp);
		void emitShift(const MachineInstr& in, U8 ext);
		void emitDiv(const MachineInstr& in, B32 isSigned);
		void emitExtBits(const MachineInstr& in, B32 sign);
		void emitCmp(const MachineInstr& in);
		void setccExt(U8 cc, Reg d);
		void emitFSign(const MachineInstr& in, B32 abs);
		void emitFCmpFlags(const MachineInstr& in);
		void emitCvt(const MachineInstr& in);
		void emitVSplat(const MachineInstr& in);
		void emitVExtract(const MachineInstr& in);
		void emitVPackLane(const MachineInstr& in);
		void fldSlot(const MachineOperand& o);
		void fstpSlot(const MachineOperand& o);
		void emitX87StoreMem(const MachineInstr& in);
		void emitX87ToInt(const MachineInstr& in);
		void emitX87FromSse(const MachineInstr& in);
		void emitX87ToSse(const MachineInstr& in);
		void emitX87Binary(const MachineInstr& in, U32 idx);
		void emitX87Cmp(const MachineInstr& in);
		void vaLoadResult(const MachineInstr& in);
		void emitVaStart(const MachineInstr& in);
		void vaFetchOverflow(I32 step, U32 align);
		void vaFetch(I32 offDisp, U32 limit, I32 regStep);
		U32 vaNoRoom(I32 offDisp, U32 limit);
		void vaCopyEightbyte(I32 offDisp, I32 regStep, I32 disp);
		void vaFetchAggregate(const MachineInstr& in);
		void emitVaArg(const MachineInstr& in);
		void copyByval(const MachineOperand& src, U32 bytes, I32 off);
		void emitCall(const MachineInstr& in);
		void emitRet();
		void emitSwitchJump(const MachineInstr& in);
		void emitBr(const MachineInstr& in, I32 fallthrough);
		void emitInst(const MachineInstr& in, I32 fallthrough);
		void prologue();
		void markCfi(CfiOp::Kind kind, PhysReg reg, I32 value);
		void markLine(U32 loc);
		void markScope(U32 loc, U32 start, U32 end);
		void recordVars(const Function& f);
		static VarLoc toVarLoc(const MachineOperand& o);
		static B32 writesSlot(const MachineInstr& in, I64 cfaOff);
		static B32 instBefore(const OpenVarRange& open, U32 inst);
		void closeRange(const OpenVarRange& open, U32 end);
		void setLoc(List<OpenVarRange>& live, U32 inst, VarLoc loc, U32 pc);
		void applyRecords(List<OpenVarRange>& live, U32 chain, U32 pc);
		void clobber(List<OpenVarRange>& live, const MachineInstr& in, U32 pc);
		static void meet(List<OpenVarRange>& into, const List<OpenVarRange>& from);
		void blockVars(const MachineBlock& blk, List<OpenVarRange>& live);
		void solveVars(const MachineFunc& f);
		void beginVars(U32 bi);
		void endVars(const MachineBlock& blk);
	private:
		std::ostream* os;
		UniquePtr<ObjectFile> obj;
		List<U8> code;
		List<AsmReloc> relocs; // text, every function so far
		List<PlacedFunc> placed;
		List<EncodedFunc> encoded;					// elf only, one per placed function
		EncodedFunc* encodedFunc = nullptr; // the function being encoded, null on coff
		U32 scope = 0;											// of the last located instr
		List<List<OpenVarRange>> varsIn; // block index -> var locations at its start, empty untracked
		List<OpenVarRange> liveVars;		 // in the block being encoded
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
	};
} // namespace rat

#endif
