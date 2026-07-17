#include "Pass/Emit/X86Emitter.h"

#include "CodeGen/MachineFunction.h"
#include "CodeGen/MachineModule.h"
#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"
#include "Target/ObjectFile.h"
#include "Target/Target.h"
#include "Target/X86Asm.h"

namespace rat {
	void X86EncodePass::emitCopy(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		if(in.regClass == detail::kFp) {
			U32 w = d.width;
			if(d.kind == MachineOperand::Kind::Phys && s.kind == MachineOperand::Kind::Phys) {
				if(xmmOf(d) != xmmOf(s))
					a->sseArith(0x10, w, xmmOf(d), xmmOf(s)); // movss/movsd reg,reg
			} else if(d.kind == MachineOperand::Kind::Phys && s.kind == MachineOperand::Kind::FrameSlot)
				a->loadXmm(xmmOf(d), RBP, s.slot, w);
			else if(d.kind == MachineOperand::Kind::FrameSlot && s.kind == MachineOperand::Kind::Phys)
				a->storeXmm(xmmOf(s), RBP, d.slot, w);
			return;
		}
		if(d.kind == MachineOperand::Kind::Phys) {
			readGp(s, gpOf(d));
		} else if(d.kind == MachineOperand::Kind::FrameSlot) {
			if(s.kind == MachineOperand::Kind::Phys)
				a->storeMem(RBP, d.slot, gpOf(s), 8);
			else {
				readGp(s, R11);
				a->storeMem(RBP, d.slot, R11, 8);
			}
		}
	}

	void X86EncodePass::emitLoadImm(const MachineInstr& in) {
		Reg d = gpOf(in.defs[0]);
		I64 v = in.uses[0].imm;
		if((U64)v <= 0xFFFFFFFFull)
			a->movRegImm32(d, (U32)v); // mov r32, imm32 zero-extends
		else if(v == (I64)(I32)v)
			a->movRegImmSext32(d, (I32)v);
		else
			a->movRegImm64(d, (U64)v);
	}

	void X86EncodePass::emitLoadSym(const MachineInstr& in) {
		a->leaRipSym(gpOf(in.defs[0]), in.uses[0].sym, 0);
	}

	void X86EncodePass::emitFrameAddr(const MachineInstr& in) {
		if(in.imm == -1) {
			Reg d = gpOf(in.defs[0]);
			readGp(in.uses[0], R10);
			a->movRegImm64(R11, ~(U64)15);
			a->addRegImm32(R10, 15);
			a->andRR(R10, R11);
			a->subRR(RSP, R10);
			a->andRR(RSP, R11);
			a->movRR(d, RSP);
			return;
		}
		a->leaMem(gpOf(in.defs[0]), RBP, (I32)in.imm);
	}

	void X86EncodePass::emitLea(const MachineInstr& in) {
		Reg base = gpOf(in.uses[0]);
		Reg index = gpOf(in.uses[1]);
		a->leaSib(gpOf(in.defs[0]), base, index, (U32)(in.imm2 & 3), (I32)in.imm);
	}

	void X86EncodePass::emitLoad(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& addr = in.uses[0];
		U32 w = d.width;
		if(addr.kind == MachineOperand::Kind::FrameSlot) {
			a->load64(gpOf(d), RBP, addr.slot);
			return;
		}
		B32 sign = (in.imm2 & 1) != 0;
		Reg base = gpOf(addr);
		if(in.imm2 & 2) { // scaled index in use[1]
			Reg index = gpOf(in.uses[1]);
			a->loadExtSib(gpOf(d), base, index, (U32)((in.imm2 >> 2) & 3), (I32)in.imm, w, sign);
			return;
		}
		a->loadExt(gpOf(d), base, (I32)in.imm, w, sign);
	}

	void X86EncodePass::emitStore(const MachineInstr& in) {
		const MachineOperand& a0 = in.uses[0];
		const MachineOperand& src = in.uses[1];
		if(a0.kind == MachineOperand::Kind::FrameSlot) {
			a->storeMem(RBP, a0.slot, gpOf(src), 8);
			return;
		}
		a->storeMem(gpOf(a0), (I32)in.imm, gpOf(src), src.width);
	}

	void X86EncodePass::emitFLoad(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& addr = in.uses[0];
		U32 w = d.width;
		if(addr.kind == MachineOperand::Kind::Imm) {
			a->movRegImm64(R11, (U64)addr.imm);
			a->storeMem(RBP, fl->ldScratch, R11, 8);
			a->loadXmm(xmmOf(d), RBP, fl->ldScratch, w);
			return;
		}
		if(addr.kind == MachineOperand::Kind::FrameSlot) {
			a->loadXmm(xmmOf(d), RBP, addr.slot, w);
			return;
		}
		if(in.imm2 & 2) { // scaled index in use[1]
			Reg index = gpOf(in.uses[1]);
			a->loadXmmSib(xmmOf(d), gpOf(addr), index, (U32)((in.imm2 >> 2) & 3), (I32)in.imm, w);
			return;
		}
		a->loadXmm(xmmOf(d), gpOf(addr), (I32)in.imm, w);
	}

	void X86EncodePass::emitFStore(const MachineInstr& in) {
		const MachineOperand& a0 = in.uses[0];
		const MachineOperand& src = in.uses[1];
		if(a0.kind == MachineOperand::Kind::FrameSlot) {
			a->storeXmm(xmmOf(src), RBP, a0.slot, src.width);
			return;
		}
		a->storeXmm(xmmOf(src), gpOf(a0), (I32)in.imm, src.width);
	}

	void X86EncodePass::emitAlu(const MachineInstr& in, U8 aluOp) {
		Reg d = gpOf(in.defs[0]);
		if(in.uses[1].kind == MachineOperand::Kind::Imm) {
			// group-1 /ext for each RR opcode byte: add 01->0, or 09->1, and 21->4, sub 29->5, xor 31->6
			U8 ext = (U8)(aluOp >> 3);
			a->aluImm(ext, d, (I32)in.uses[1].imm);
			return;
		}
		a->aluRR(aluOp, gpOf(in.defs[0]), gpOf(in.uses[1]));
	}

	void X86EncodePass::emitMul(const MachineInstr& in) {
		if(in.uses[1].kind == MachineOperand::Kind::Imm) {
			a->imulRRI(gpOf(in.defs[0]), gpOf(in.uses[0]), (I32)in.uses[1].imm);
			return;
		}
		a->imulRR(gpOf(in.defs[0]), gpOf(in.uses[1]));
	}

	void X86EncodePass::emitNegNot(const MachineInstr& in, B32 neg) {
		Reg d = gpOf(in.defs[0]);
		if(neg)
			a->negReg(d);
		else
			a->notReg(d);
	}

	void X86EncodePass::emitShift(const MachineInstr& in, U8 ext) {
		if(in.uses.size() > 1 && in.uses[1].kind == MachineOperand::Kind::Imm) {
			a->shiftImm(ext, gpOf(in.defs[0]), (U8)(in.uses[1].imm & 63));
			return;
		}
		a->shiftCL(ext, gpOf(in.defs[0]));
	}

	void X86EncodePass::emitDiv(const MachineInstr& in, B32 isSigned) {
		B32 wide = (U32)in.imm > 32;
		if(isSigned) {
			a->cqoW(wide);
			a->idivRegW(RCX, wide);
		} else {
			a->xorSelf(RDX);
			a->divRegW(RCX, wide);
		}
		if(!wide) {
			a->movsxd32(RAX, RAX);
			a->movsxd32(RDX, RDX);
		}
	}

	void X86EncodePass::emitMaskBits(const MachineInstr& in) {
		U32 bits = (U32)in.imm;
		if(bits == 0 || bits >= 64)
			return;
		Reg d = gpOf(in.defs[0]);
		if(bits == 32) {
			a->movRR32(d, d); // 32-bit self-move zero-extends
			return;
		}
		if(bits < 32) {
			a->aluImm(4, d, (I32)(((U32)1 << bits) - 1)); // and d, imm
			return;
		}
		a->movRegImm64(R11, ((U64)1 << bits) - 1);
		a->andRR(d, R11);
	}

	void X86EncodePass::emitSignExtBits(const MachineInstr& in) {
		U32 bits = (U32)in.imm;
		if(bits == 0 || bits >= 64)
			return;
		Reg d = gpOf(in.defs[0]);
		if(bits == 32) {
			a->movsxd32(d, d);
			return;
		}
		U8 sh = (U8)(64 - bits);
		a->shiftImm(4, d, sh); // shl
		a->shiftImm(7, d, sh); // sar
	}

	void X86EncodePass::emitCmp(const MachineInstr& in) {
		if(in.uses[1].kind == MachineOperand::Kind::Imm) {
			a->cmpRegImm32(gpOf(in.uses[0]), (I32)in.uses[1].imm);
			return;
		}
		a->cmpRR(gpOf(in.uses[0]), gpOf(in.uses[1]));
	}

	void X86EncodePass::emitSetCC(const MachineInstr& in) {
		Reg d = gpOf(in.defs[0]);
		a->setcc((U8)in.imm, d);
		a->movzxByte(d, d);
	}

	void X86EncodePass::emitFArith(const MachineInstr& in, U8 op) {
		U32 w = (U32)in.imm;
		a->sseArith(op, w, xmmOf(in.defs[0]), xmmOf(in.uses[1]));
	}

	void X86EncodePass::emitFNeg(const MachineInstr& in) {
		U32 w = (U32)in.imm;
		U32 d = xmmOf(in.defs[0]);
		U32 s = xmmOf(in.uses[0]);
		U32 z = winAbi ? 5 : 15; // scratch xmm; xmm15 is callee-saved on winAbi
		a->pxor(z, z);
		a->sseArith(0x5c, w, z, s == d ? d : s);
		if(d != z)
			a->sseArith(0x10, w, d, z);
	}

	void X86EncodePass::emitFCmp(const MachineInstr& in) {
		U32 w = in.uses[0].width;
		U32 lhs = xmmOf(in.uses[0]);
		U32 rhs = xmmOf(in.uses[1]);
		if(in.imm2)
			a->ucomis(w, rhs, lhs);
		else
			a->ucomis(w, lhs, rhs);
		Reg d = gpOf(in.defs[0]);
		a->setcc((U8)in.imm, d);
		a->movzxByte(d, d);
	}

	void X86EncodePass::emitCvt(const MachineInstr& in) {
		U8 pfx = (U8)((in.imm >> 16) & 0xff);
		U8 opc = (U8)((in.imm >> 8) & 0xff);
		B32 w = (in.imm & 1) != 0;
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		U32 dst = (in.regClass == detail::kGp) ? (U32)gpOf(d) : xmmOf(d);
		U32 srcReg;
		if(in.regClass == detail::kGp)
			srcReg = xmmOf(s); // FPToSI/UI: xmm -> gp
		else if(X86Target::isXmm(s.phys))
			srcReg = xmmOf(s); // fp -> fp
		else
			srcReg = (U32)gpOf(s); // SIToFP/UIToFP: gp -> xmm
		a->cvtRR(pfx, opc, w, dst, srcReg);
	}

	void X86EncodePass::fldSlot(I32 slot) { a->fldT(RBP, slot); }
	void X86EncodePass::fstpSlot(I32 slot) { a->fstpT(RBP, slot); }

	void X86EncodePass::emitX87LoadMem(const MachineInstr& in) {
		if(in.imm == -1) {
			fldSlot(in.uses[0].slot);
			return;
		}
		Reg base = gpOf(in.uses[0]);
		a->fldT(base, 0);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87StoreMem(const MachineInstr& in) {
		if(in.imm == -1) {
			fstpSlot(in.defs[0].slot);
			return;
		}
		if(in.imm == -2) {
			a->fstpReg0();
			return;
		}
		Reg base = gpOf(in.uses[0]);
		fldSlot(in.uses[1].slot);
		a->fstpT(base, 0);
	}

	void X86EncodePass::emitX87LoadImmD(const MachineInstr& in) {
		a->movRegImm64(R11, (U64)in.uses[0].imm);
		a->storeMem(RBP, fl->ldScratch, R11, 8);
		a->fldL(RBP, fl->ldScratch);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87FromInt(const MachineInstr& in) {
		readGp(in.uses[0], R11);
		a->storeMem(RBP, fl->ldScratch, R11, 8);
		a->fildQ(RBP, fl->ldScratch);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87ToInt(const MachineInstr& in) {
		fldSlot(in.uses[0].slot);
		a->fnstcw(RBP, fl->ldScratch + 8);
		a->loadExt(R10, RBP, fl->ldScratch + 8, 2, false);
		a->movRegImm64(R11, 0x0c00);
		a->orRR(R10, R11);
		a->storeMem(RBP, fl->ldScratch + 10, R10, 2);
		a->fldcw(RBP, fl->ldScratch + 10);
		a->fistpQ(RBP, fl->ldScratch);
		a->fldcw(RBP, fl->ldScratch + 8);
		a->load64(gpOf(in.defs[0]), RBP, fl->ldScratch);
	}

	void X86EncodePass::emitX87FromSse(const MachineInstr& in) {
		if(in.imm == 80) {
			fldSlot(in.uses[0].slot);
			fstpSlot(in.defs[0].slot);
			return;
		}
		U32 sw = (U32)in.imm;
		a->storeXmm(xmmOf(in.uses[0]), RBP, fl->ldScratch, sw);
		if(sw == 4)
			a->fldD(RBP, fl->ldScratch);
		else
			a->fldL(RBP, fl->ldScratch);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87ToSse(const MachineInstr& in) {
		U32 dw = (U32)in.imm;
		fldSlot(in.uses[0].slot);
		if(dw == 4)
			a->fstpD(RBP, fl->ldScratch);
		else
			a->fstpL(RBP, fl->ldScratch);
		a->loadXmm(xmmOf(in.defs[0]), RBP, fl->ldScratch, dw);
	}

	void X86EncodePass::emitX87Binary(const MachineInstr& in, U32 idx) {
		fldSlot(in.uses[0].slot);
		fldSlot(in.uses[1].slot);
		static void (Asm::*const kArith[])() = {&Asm::faddp, &Asm::fsubp, &Asm::fmulp, &Asm::fdivp};
		(a->*kArith[idx])();
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87Neg(const MachineInstr& in) {
		fldSlot(in.uses[0].slot);
		a->fchs();
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87Cmp(const MachineInstr& in) {
		B32 swap = in.imm2 != 0;
		if(swap) {
			fldSlot(in.uses[0].slot); // -> st(1)
			fldSlot(in.uses[1].slot); // -> st(0)
		} else {
			fldSlot(in.uses[1].slot); // -> st(1)
			fldSlot(in.uses[0].slot); // -> st(0)
		}
		a->fucomip();
		a->fstpReg0();
		Reg d = gpOf(in.defs[0]);
		a->setcc((U8)in.imm, d);
		a->movzxByte(d, d);
	}
} // namespace rat
