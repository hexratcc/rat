#include "pass/emit/x86/x86_encode.h"

#include "codegen/machine_function.h"

namespace rat {
	void X86EncodePass::emitCopy(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		assert(d.isPhys() && s.isPhys() && "regalloc leaves only reg to reg copies");
		if(X86Target::isXmm(d.phys))
			return copyXmm(xmmOf(d), xmmOf(s));
		readGp(s, gpOf(d));
	}

	void X86EncodePass::movImm32(Reg r, U32 v) {
		a->opReg(enc::kMovImm32, r);
		a->d32(v);
	}

	// mov r32 zero-extends, mov r/m64 sign-extends an imm32, else the full imm64
	void X86EncodePass::emitLoadImm(const MachineInstr& in) {
		Reg d = gpOf(in.defs[0]);
		I64 v = in.uses[0].imm;
		if((U64)v <= 0xFFFFFFFFull) {
			movImm32(d, (U32)v);
		} else if(v == (I64)(I32)v) {
			a->rr({0, 0, 0xc7, 1}, 0, d);
			a->d32((U32)v);
		} else {
			a->movImm64(d, (U64)v);
		}
	}

	void X86EncodePass::emitStackAlloc(const MachineInstr& in) {
		readGp(in.uses[0], R10);
		a->movImm64(R11, ~(U64)15);
		a->aluImm(ALU_ADD, R10, 15);
		a->rr(enc::kAndW, R11, R10);
		a->rr(enc::kSubW, R10, RSP);
		a->rr(enc::kAndW, R11, RSP);
		a->rr(enc::kMovW, RSP, gpOf(in.defs[0]));
	}

	// buf[0] = rbp, buf[1] = resume address, buf[2] = rsp
	void X86EncodePass::emitSetJmp(const MachineInstr& in) {
		Reg buf = gpOf(in.uses[0]);
		U32 leaAt = a->rip(enc::kLeaW, R10);
		a->store({buf, 0}, RBP, 8);
		a->store({buf, 8}, R10, 8);
		a->store({buf, 16}, RSP, 8);
		movImm32(RAX, 0);
		U32 done = a->jmpRel32();
		a->patchRel32(leaAt, a->here()); // resume label
		movImm32(RAX, 1);
		a->patchRel32(done, a->here());
	}

	void X86EncodePass::emitLongJmp(const MachineInstr& in) {
		Reg buf = gpOf(in.uses[0]);
		a->load(R10, {buf, 8}, 8, false); // read the resume address before rsp moves
		a->load(RBP, {buf, 0}, 8, false);
		a->load(RSP, {buf, 16}, 8, false);
		a->rr(enc::kGroup5, 4, R10); // jmp
	}

	void X86EncodePass::emitStore(const MachineInstr& in) {
		const MachineOperand& src = in.uses[1];
		Mem mem = memOf(in, 2);
		assert((!in.uses[0].isFrameSlot() || src.width == 8) && "a slot store moves 8 bytes");
		if(src.isImm())
			return a->storeImm(mem, src.imm, src.width);
		a->store(mem, gpOf(src), src.width);
	}

	void X86EncodePass::emitFLoad(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& addr = in.uses[0];
		if(addr.kind == MachineOperand::Kind::Sym)
			return a->ripSym({Asm::ssePrefixByte(d.width), 0x0f, 0x10}, xmmOf(d), addr.sym());
		a->loadXmm(xmmOf(d), memOf(in, 1), d.width);
	}

	// aluOp is the r/m, r opcode, its group-1 /digit is aluOp >> 3
	void X86EncodePass::emitAlu(const MachineInstr& in, U8 aluOp) {
		assert((in.defs.empty() ||
						(in.defs[0].phys == in.uses[0].phys && in.defs[0].width == in.uses[0].width)) &&
					 "alu def is tied to use 0");
		Reg d = gpOf(in.uses[0]);
		B32 w = in.uses[0].width == 8;
		if(in.uses[1].isImm())
			return a->aluImm((Alu)(aluOp >> 3), d, (I32)in.uses[1].imm, w);
		a->rr({0, 0, aluOp, w}, gpOf(in.uses[1]), d);
	}

	void X86EncodePass::emitDiv(const MachineInstr& in, B32 isSigned) {
		B32 wide = (U32)in.imm > 32;
		if(isSigned)
			a->opReg({0, 0, 0x99, wide}, 0); // cdq, cqo
		else
			a->rr(enc::kXorW, RDX, RDX);
		a->rr({0, 0, 0xf7, wide}, isSigned ? 7 : 6, RCX); // idiv, div
	}

	void X86EncodePass::emitExtBits(const MachineInstr& in, B32 sign) {
		U32 bits = (U32)in.imm;
		if(bits == 0 || bits >= 64)
			return;
		Reg d = gpOf(in.defs[0]);
		if(sign && bits == 32)
			return a->rr(enc::kMovsxdW, d, d);
		if(bits == 8 || bits == 16) { // movsx r64, movzx r32
			U8 opc = (U8)((sign ? 0xbe : 0xb6) + (bits == 16));
			return a->rr({0, 0x0f, opc, sign, bits == 8 && Asm::needsRex8(d)}, d, d);
		}
		if(sign) {
			U8 sh = (U8)(64 - bits);
			shiftImm(4, d, sh, true);
			return shiftImm(7, d, sh, true);
		}
		if(bits == 32)
			return a->rr({0, 0, 0x89}, d, d); // a 32-bit self-move zero-extends
		if(bits < 32)
			return a->aluImm(ALU_AND, d, (I32)(((U32)1 << bits) - 1));
		a->movImm64(R11, ((U64)1 << bits) - 1);
		a->rr(enc::kAndW, R11, d);
	}

	void X86EncodePass::testReg(const MachineOperand& o) {
		Reg r = gpOf(o);
		a->rr({0, 0, 0x85, o.width == 8}, r, r);
	}

	void X86EncodePass::setccExt(U8 cc, Reg d) {
		a->rr({0, 0x0f, (U8)(0x90 + cc), 0, Asm::needsRex8(d)}, 0, d);
		a->rr({0, 0x0f, 0xb6, 1}, d, d);
	}

	U8 X86EncodePass::laneSel(U32 lane, U32 esz) {
		if(esz == 4)
			return (U8)(lane | (lane << 2) | (lane << 4) | (lane << 6));
		U32 lo = 2 * lane;
		return (U8)(lo | ((lo + 1) << 2) | (lo << 4) | ((lo + 1) << 6));
	}

	void X86EncodePass::pshufd(U32 d, U32 s, U8 sel) {
		a->rr(enc::kPshufd, d, s);
		a->b(sel);
	}

	void X86EncodePass::emitVSplat(const MachineInstr& in) {
		U32 d = xmmOf(in.defs[0]);
		U32 esz = (U32)in.imm;
		if(!in.imm2) // the lane sits in an xmm
			return pshufd(d, xmmOf(in.uses[0]), laneSel(0, esz));
		Enc gpToXmm = esz == 8 ? enc::kMovqToXmm : enc::kMovdToXmm;
		a->rr(gpToXmm, d, gpOf(in.uses[0]));
		pshufd(d, d, laneSel(0, esz));
	}

	void X86EncodePass::emitVExtract(const MachineInstr& in) {
		U32 lane = (U32)in.imm;
		U32 esz = (U32)((U64)in.imm2 >> 1);
		U32 src = xmmOf(in.uses[0]);
		if((in.imm2 & 1) == 0)
			return pshufd(xmmOf(in.defs[0]), src, laneSel(lane, esz));
		Reg d = gpOf(in.defs[0]);
		if(lane == 0)
			return a->rr({0x66, 0x0f, 0x7e, esz == 8}, src, d); // movd, movq
		a->storeXmm(src, {RBP, fl->vecScratch}, 16);
		a->load(d, {RBP, fl->vecScratch + (I32)(lane * esz)}, esz, true);
	}

	void X86EncodePass::copyXmm(U32 d, U32 s) {
		if(d != s)
			a->rr(enc::kMovaps, d, s);
	}

	void X86EncodePass::shiftImm(U8 ext, Reg r, U8 count, B32 w) {
		a->rr({0, 0, 0xc1, w}, ext, r);
		a->b(count);
	}

	void X86EncodePass::sseShift(U32 laneBits, U8 ext, U32 x, U8 count) {
		U8 group = laneBits == 16 ? 0x71 : 0x72; // the word and dword shifts
		if(laneBits == 64)
			group = 0x73;
		a->rr({0x66, 0x0f, group}, ext, x);
		a->b(count);
	}

	void X86EncodePass::emitFSign(const MachineInstr& in, B32 abs) {
		U32 w = (U32)in.imm;
		U32 d = xmmOf(in.defs[0]);
		U32 s = xmmOf(in.uses[0]);
		if(abs) {
			copyXmm(d, s);
			sseShift(w * 8, 6, d, 1);				 // shl
			return sseShift(w * 8, 2, d, 1); // shr
		}
		U32 z = conv->sseVolatileCount - 1; // top volatile xmm is encoder scratch
		// flip the sign bit, 0-x would turn -0.0 into +0.0 and quiet a NaN
		a->rr({0x66, 0x0f, 0x76}, z, z); // pcmpeqd, all ones
		sseShift(w * 8, 6, z, (U8)(w * 8 - 1));
		copyXmm(d, s);
		a->rr({0x66, 0x0f, 0xef}, d, z); // pxor
	}

	void X86EncodePass::emitFCmpFlags(const MachineInstr& in) {
		U32 s = in.imm2 != 0;
		U8 pfx = in.uses[0].width == 8 ? 0x66 : 0;
		a->rr({pfx, 0x0f, 0x2e}, xmmOf(in.uses[s]), xmmOf(in.uses[1 - s])); // ucomiss, ucomisd
	}

	// esc and ext pick the x87 memory op, fld m80 is db /5
	void X86EncodePass::x87Mem(U8 esc, U8 ext, const Mem& mem) { a->rm({0, 0, esc}, ext, mem); }
	void X86EncodePass::fldSlot(const MachineOperand& o) { x87Mem(0xdb, 5, {RBP, o.slot}); }
	void X86EncodePass::fstpSlot(const MachineOperand& o) { x87Mem(0xdb, 7, {RBP, o.slot}); }

	void X86EncodePass::twoBytes(U8 b0, U8 b1) {
		a->b(b0);
		a->b(b1);
	}

	void X86EncodePass::emitX87StoreMem(const MachineInstr& in) {
		if(in.imm == -1)
			return fstpSlot(in.defs[0]);
		if(in.imm == -2)
			return twoBytes(0xdd, 0xd8); // fstp st0
		fldSlot(in.uses[1]);
		x87Mem(0xdb, 7, {gpOf(in.uses[0]), 0});
	}

	// fistp under a round-toward-zero control word
	void X86EncodePass::emitX87ToInt(const MachineInstr& in) {
		fldSlot(in.uses[0]);
		x87Mem(0xd9, 7, {RBP, fl->ldScratch + 8}); // fnstcw
		a->load(R10, {RBP, fl->ldScratch + 8}, 2, false);
		a->movImm64(R11, 0x0c00);
		a->rr(enc::kOrW, R11, R10);
		a->store({RBP, fl->ldScratch + 10}, R10, 2);
		x87Mem(0xd9, 5, {RBP, fl->ldScratch + 10}); // fldcw
		x87Mem(0xdf, 7, {RBP, fl->ldScratch});			// fistp m64
		x87Mem(0xd9, 5, {RBP, fl->ldScratch + 8});
		a->load(gpOf(in.defs[0]), {RBP, fl->ldScratch}, 8, false);
	}

	// fld m32fp is d9 /0, m64fp dd /0
	void X86EncodePass::emitX87FromSse(const MachineInstr& in) {
		if(in.imm == detail::kX87MemBits) {
			fldSlot(in.uses[0]);
		} else {
			a->storeXmm(xmmOf(in.uses[0]), {RBP, fl->ldScratch}, (U32)in.imm);
			x87Mem(in.imm == 4 ? 0xd9 : 0xdd, 0, {RBP, fl->ldScratch});
		}
		fstpSlot(in.defs[0]);
	}

	void X86EncodePass::emitX87Binary(const MachineInstr& in, U32 idx) {
		static constexpr U8 kArith[] = {0xc1, 0xe9, 0xc9, 0xf9}; // faddp fsubp fmulp fdivp
		fldSlot(in.uses[0]);
		fldSlot(in.uses[1]);
		twoBytes(0xde, kArith[idx]);
		fstpSlot(in.defs[0]);
	}

	void X86EncodePass::emitX87Cmp(const MachineInstr& in) {
		U32 s = in.imm2 != 0;
		fldSlot(in.uses[1 - s]); // -> st(1)
		fldSlot(in.uses[s]);		 // -> st(0)
		twoBytes(0xdf, 0xe9);		 // fucomip
		twoBytes(0xdd, 0xd8);		 // fstp st0
		setccExt((U8)in.imm, gpOf(in.defs[0]));
	}
} // namespace rat
