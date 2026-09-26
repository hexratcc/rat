#include "pass/emit/x86/x86_encode.h"

#include "codegen/machine_function.h"
#include "target/x86/x86_asm.h"

namespace rat {
	void X86EncodePass::emitCopy(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		if(in.regClass == detail::kFp) {
			if(d.isPhys() && s.isPhys())
				copyXmm(xmmOf(d), xmmOf(s));
			else if(d.isPhys() && s.kind == MachineOperand::Kind::FrameSlot)
				a->loadXmm(xmmOf(d), RBP, s.slot, d.width);
			else if(d.kind == MachineOperand::Kind::FrameSlot && s.isPhys())
				a->storeXmm(xmmOf(s), RBP, d.slot, d.width);
			return;
		}
		if(d.isPhys()) {
			readGp(s, gpOf(d));
		} else if(d.kind == MachineOperand::Kind::FrameSlot) {
			Reg r = s.isPhys() ? gpOf(s) : R11;
			readGp(s, r);
			a->storeMem(RBP, d.slot, r, 8);
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

	void X86EncodePass::emitStackAlloc(const MachineInstr& in) {
		readGp(in.uses[0], R10);
		a->movRegImm64(R11, ~(U64)15);
		a->addRegImm32(R10, 15);
		a->andRR(R10, R11);
		a->subRR(RSP, R10);
		a->andRR(RSP, R11);
		a->movRR(gpOf(in.defs[0]), RSP);
	}

	// buf[0] = rbp, buf[1] = resume address, buf[2] = rsp
	void X86EncodePass::emitSetJmp(const MachineInstr& in) {
		Reg buf = gpOf(in.uses[0]);
		U32 leaAt = a->leaRipDisp(R10);
		a->storeMem(buf, 0, RBP, 8);
		a->storeMem(buf, 8, R10, 8);
		a->storeMem(buf, 16, RSP, 8);
		a->movRegImm32(RAX, 0);
		U32 done = a->jmpRel32();
		a->patchRel32(leaAt, a->here()); // resume label
		a->movRegImm32(RAX, 1);
		a->patchRel32(done, a->here());
	}

	void X86EncodePass::emitLongJmp(const MachineInstr& in) {
		Reg buf = gpOf(in.uses[0]);
		a->load64(R10, buf, 8); // read the resume address before rsp moves
		a->load64(RBP, buf, 0);
		a->load64(RSP, buf, 16);
		a->jmpReg(R10);
	}

	void X86EncodePass::emitLoad(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& addr = in.uses[0];
		U32 w = d.width;
		if(addr.kind == MachineOperand::Kind::FrameSlot)
			return a->load64(gpOf(d), RBP, addr.slot);
		B32 sign = (in.imm2 & 1) != 0;
		Reg base = gpOf(addr);
		if(in.imm2 & 2) // scaled index in use[1]
			return a->loadExtSib(gpOf(d), base, gpOf(in.uses[1]), scaleOf(in), (I32)in.imm, w, sign);
		a->loadExt(gpOf(d), base, (I32)in.imm, w, sign);
	}

	void X86EncodePass::emitStore(const MachineInstr& in) {
		const MachineOperand& a0 = in.uses[0];
		const MachineOperand& src = in.uses[1];
		if(a0.kind == MachineOperand::Kind::FrameSlot) {
			if(src.kind == MachineOperand::Kind::Imm)
				return a->storeMemImm(RBP, a0.slot, src.imm, 8);
			return a->storeMem(RBP, a0.slot, gpOf(src), 8);
		}
		if(in.imm2 & 2) { // scaled index in use[2]
			Reg index = gpOf(in.uses[2]);
			if(src.kind == MachineOperand::Kind::Imm)
				return a->storeMemImmSib(gpOf(a0), index, scaleOf(in), (I32)in.imm, src.imm, src.width);
			return a->storeMemSib(gpOf(a0), index, scaleOf(in), (I32)in.imm, gpOf(src), src.width);
		}
		if(src.kind == MachineOperand::Kind::Imm)
			return a->storeMemImm(gpOf(a0), (I32)in.imm, src.imm, src.width);
		a->storeMem(gpOf(a0), (I32)in.imm, gpOf(src), src.width);
	}

	void X86EncodePass::emitFLoad(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& addr = in.uses[0];
		U32 w = d.width;
		if(addr.kind == MachineOperand::Kind::Sym)
			return a->loadXmmRipSym(xmmOf(d), addr.sym(), w);
		if(addr.kind == MachineOperand::Kind::Imm) {
			immToScratch(addr.imm);
			return a->loadXmm(xmmOf(d), RBP, fl->ldScratch, w);
		}
		if(addr.kind == MachineOperand::Kind::FrameSlot)
			return a->loadXmm(xmmOf(d), RBP, addr.slot, w);
		if(in.imm2 & 2) // scaled index in use[1]
			return a->loadXmmSib(xmmOf(d), gpOf(addr), gpOf(in.uses[1]), scaleOf(in), (I32)in.imm, w);
		a->loadXmm(xmmOf(d), gpOf(addr), (I32)in.imm, w);
	}

	void X86EncodePass::emitFStore(const MachineInstr& in) {
		const MachineOperand& a0 = in.uses[0];
		const MachineOperand& src = in.uses[1];
		if(a0.kind == MachineOperand::Kind::FrameSlot)
			return a->storeXmm(xmmOf(src), RBP, a0.slot, src.width);
		if(in.imm2 & 2) { // scaled index in use[2]
			Reg index = gpOf(in.uses[2]);
			return a->storeXmmSib(xmmOf(src), gpOf(a0), index, scaleOf(in), (I32)in.imm, src.width);
		}
		a->storeXmm(xmmOf(src), gpOf(a0), (I32)in.imm, src.width);
	}

	void X86EncodePass::emitAlu(const MachineInstr& in, U8 aluOp) {
		Reg d = gpOf(in.defs[0]);
		// group-1 /ext for each RR opcode byte: add 01->0, or 09->1, and 21->4, sub 29->5, xor 31->6
		if(in.uses[1].kind == MachineOperand::Kind::Imm)
			return a->aluImm((U8)(aluOp >> 3), d, (I32)in.uses[1].imm);
		a->aluRR(aluOp, d, gpOf(in.uses[1]));
	}

	void X86EncodePass::emitShift(const MachineInstr& in, U8 ext) {
		if(in.uses.size() > 1 && in.uses[1].kind == MachineOperand::Kind::Imm)
			return a->shiftImm(ext, gpOf(in.defs[0]), (U8)(in.uses[1].imm & 63));
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

	void X86EncodePass::emitExtBits(const MachineInstr& in, B32 sign) {
		U32 bits = (U32)in.imm;
		if(bits == 0 || bits >= 64)
			return;
		Reg d = gpOf(in.defs[0]);
		if(sign && bits == 32)
			return a->movsxd32(d, d);
		if(sign) {
			U8 sh = (U8)(64 - bits);
			a->shiftImm(4, d, sh);				// shl
			return a->shiftImm(7, d, sh); // sar
		}
		if(bits == 32)
			return a->movRR32(d, d); // 32-bit self-move zero-extends
		if(bits < 32)
			return a->aluImm(4, d, (I32)(((U32)1 << bits) - 1)); // and d, imm
		a->movRegImm64(R11, ((U64)1 << bits) - 1);
		a->andRR(d, R11);
	}

	void X86EncodePass::emitCmp(const MachineInstr& in) {
		Reg l = gpOf(in.uses[0]);
		if(in.uses[1].kind != MachineOperand::Kind::Imm)
			return a->cmpRR(l, gpOf(in.uses[1]));
		if(in.uses[1].imm == 0)
			return a->testRR(l, l); // shorter encoding, same flags for eq/ne/sign
		a->cmpRegImm32(l, (I32)in.uses[1].imm);
	}

	void X86EncodePass::setccExt(U8 cc, Reg d) {
		a->setcc(cc, d);
		a->movzxByte(d, d);
	}

	U8 X86EncodePass::laneSel(U32 lane, U32 esz) {
		if(esz == 4)
			return (U8)(lane | (lane << 2) | (lane << 4) | (lane << 6));
		U32 lo = 2 * lane;
		return (U8)(lo | ((lo + 1) << 2) | (lo << 4) | ((lo + 1) << 6));
	}

	void X86EncodePass::emitVSplat(const MachineInstr& in) {
		U32 d = xmmOf(in.defs[0]);
		U32 esz = (U32)in.imm;
		if(in.imm2) { // integer lane arrives in a gp register
			a->movdXmmGp(d, gpOf(in.uses[0]), esz == 8);
			return a->pshufd(d, d, laneSel(0, esz));
		}
		a->pshufd(d, xmmOf(in.uses[0]), laneSel(0, esz));
	}

	void X86EncodePass::emitVExtract(const MachineInstr& in) {
		U32 lane = (U32)in.imm;
		U32 esz = (U32)((U64)in.imm2 >> 1);
		U32 src = xmmOf(in.uses[0]);
		if((in.imm2 & 1) == 0)
			return a->pshufd(xmmOf(in.defs[0]), src, laneSel(lane, esz));
		Reg d = gpOf(in.defs[0]);
		if(lane == 0) {
			a->movGpXmm(d, src, esz == 8);
			if(esz != 8)
				a->movsxd32(d, d); // sign-extend the zero-extended dword
			return;
		}
		// staged through the 16-byte scratch slot
		a->storeXmm(src, RBP, fl->vecScratch, 16);
		a->loadExt(d, RBP, fl->vecScratch + (I32)(lane * esz), esz, true);
	}

	// the lanes are gathered through the 16-byte vec scratch slot (float, or int without
	// sse4.1), one lane per instruction
	void X86EncodePass::emitVPackLane(const MachineInstr& in) {
		U32 esz = (U32)in.imm;
		I32 disp = fl->vecScratch + (I32)(((U32)in.imm2 >> 1) * esz);
		if(in.imm2 & 1)
			a->storeMem(RBP, disp, gpOf(in.uses[0]), esz);
		else
			a->storeXmm(xmmOf(in.uses[0]), RBP, disp, esz);
	}

	void X86EncodePass::copyXmm(U32 d, U32 s) {
		if(d != s)
			a->movaps(d, s);
	}

	void X86EncodePass::emitFSign(const MachineInstr& in, B32 abs) {
		U32 w = (U32)in.imm;
		U32 d = xmmOf(in.defs[0]);
		U32 s = xmmOf(in.uses[0]);
		if(abs) {
			copyXmm(d, s);
			a->sseShiftImm(w * 8, 6, d, 1);
			return a->sseShiftImm(w * 8, 2, d, 1);
		}
		U32 z = conv->sseVolatileCount - 1; // top volatile xmm is encoder scratch
		// flip the sign bit, 0-x would turn -0.0 into +0.0 and quiet a NaN
		a->pcmpeqd(z, z);
		a->sseShiftImm(w * 8, 6, z, (U8)(w * 8 - 1));
		copyXmm(d, s);
		a->pxor(d, z);
	}

	void X86EncodePass::emitFCmpFlags(const MachineInstr& in) {
		U32 s = in.imm2 != 0;
		a->ucomis(in.uses[0].width, xmmOf(in.uses[s]), xmmOf(in.uses[1 - s]));
	}

	void X86EncodePass::emitCvt(const MachineInstr& in) {
		U8 pfx = (U8)((in.imm >> 16) & 0xff);
		U8 opc = (U8)((in.imm >> 8) & 0xff);
		B32 w = (in.imm & 1) != 0;
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		U32 dst = (in.regClass == detail::kGp) ? (U32)gpOf(d) : xmmOf(d);
		// src is xmm for fp->fp and xmm->gp (FPToSI/UI); gp only for gp->xmm (SIToFP/UIToFP)
		U32 srcReg = (U32)gpOf(s);
		if(in.regClass == detail::kGp || X86Target::isXmm(s.phys))
			srcReg = xmmOf(s);
		a->cvtRR(pfx, opc, w, dst, srcReg);
	}

	void X86EncodePass::fldSlot(const MachineOperand& o) { a->fldT(RBP, o.slot); }
	void X86EncodePass::fstpSlot(const MachineOperand& o) { a->fstpT(RBP, o.slot); }

	void X86EncodePass::emitX87StoreMem(const MachineInstr& in) {
		if(in.imm == -1)
			return fstpSlot(in.defs[0]);
		if(in.imm == -2)
			return a->fstpReg0();
		fldSlot(in.uses[1]);
		a->fstpT(gpOf(in.uses[0]), 0);
	}

	void X86EncodePass::emitX87ToInt(const MachineInstr& in) {
		fldSlot(in.uses[0]);
		a->fnstcw(RBP, fl->ldScratch + 8);
		a->loadExt(R10, RBP, fl->ldScratch + 8, 2, false);
		a->movRegImm64(R11, 0x0c00);
		a->orRR(R10, R11);
		a->storeMem(RBP, fl->ldScratch + 10, R10, 2);
		a->fldcw(RBP, fl->ldScratch + 10);
		a->fistpQ(RBP, fl->ldScratch);
		a->fldcw(RBP, fl->ldScratch + 8);
		unstash(gpOf(in.defs[0]));
	}

	void X86EncodePass::emitX87FromSse(const MachineInstr& in) {
		if(in.imm == 80) {
			fldSlot(in.uses[0]);
			fstpSlot(in.defs[0]);
			return;
		}
		U32 sw = (U32)in.imm;
		a->storeXmm(xmmOf(in.uses[0]), RBP, fl->ldScratch, sw);
		if(sw == 4)
			a->fldD(RBP, fl->ldScratch);
		else
			a->fldL(RBP, fl->ldScratch);
		fstpSlot(in.defs[0]);
	}

	void X86EncodePass::emitX87ToSse(const MachineInstr& in) {
		U32 dw = (U32)in.imm;
		fldSlot(in.uses[0]);
		if(dw == 4)
			a->fstpD(RBP, fl->ldScratch);
		else
			a->fstpL(RBP, fl->ldScratch);
		a->loadXmm(xmmOf(in.defs[0]), RBP, fl->ldScratch, dw);
	}

	void X86EncodePass::emitX87Binary(const MachineInstr& in, U32 idx) {
		fldSlot(in.uses[0]);
		fldSlot(in.uses[1]);
		static const U8 kArith[] = {0xc1, 0xe9, 0xc9, 0xf9}; // faddp fsubp fmulp fdivp
		a->fArithP(kArith[idx]);
		fstpSlot(in.defs[0]);
	}

	void X86EncodePass::emitX87Cmp(const MachineInstr& in) {
		U32 s = in.imm2 != 0;
		fldSlot(in.uses[1 - s]); // -> st(1)
		fldSlot(in.uses[s]);		 // -> st(0)
		a->fucomip();
		a->fstpReg0();
		setccExt((U8)in.imm, gpOf(in.defs[0]));
	}
} // namespace rat
