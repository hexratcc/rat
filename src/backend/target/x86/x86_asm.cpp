#include "target/x86/x86_asm.h"

#include "byte_io.h"

namespace rat {
	const X86CallConv& x86CallConv(OS os) {
		if(os == OS::Windows)
			return abi::kWin64;
		return abi::kSysV;
	}

	X86ArgAssigner::Loc X86ArgAssigner::next(Kind k) {
		if(k == Kind::X87) {
			Loc l{-1, stackBytes};
			stackBytes += 16;
			return l;
		}
		if(conv.sharedSlots) {
			Loc l{slot < conv.gpArgCount ? (I32)slot : -1, stackBytes};
			if(l.reg < 0)
				stackBytes += 8;
			++slot;
			return l;
		}
		U32& used = k == Kind::Sse ? sseUsed : gpUsed;
		U32 cap = k == Kind::Sse ? conv.sseArgCount : conv.gpArgCount;
		if(used < cap)
			return {(I32)used++, 0};
		Loc l{-1, stackBytes};
		stackBytes += 8;
		return l;
	}

	U32 Asm::here() const { return (U32)code.size(); }
	void Asm::b(U8 v) { code.push_back(v); }
	void Asm::d32(U32 v) { le::put32(code, v); }
	void Asm::d64(U64 v) { le::put64(code, v); }

	U8 Asm::rexByte(B32 w, U32 r, U32 x, U32 rm) {
		return (U8)(0x40 | (w ? 8 : 0) | ((r >> 3) << 2) | ((x >> 3) << 1) | (rm >> 3));
	}

	void Asm::rex(B32 w, U32 r, U32 x, U32 rm) {
		U8 v = rexByte(w, r, x, rm);
		if(v != 0x40)
			b(v);
	}

	void Asm::rexForce(B32 w, U32 r, U32 x, U32 rm) { b(rexByte(w, r, x, rm)); }

	void Asm::modrmReg(U32 reg, U32 rm) { b((U8)(0xc0 | ((reg & 7) << 3) | (rm & 7))); }

	void Asm::modrmMem(U32 reg, U32 base, I32 disp) {
		b((U8)(0x80 | ((reg & 7) << 3) | (base & 7)));
		if((base & 7) == 4)
			b(0x24); // SIB: scale=0, index=none, base
		d32((U32)disp);
	}

	void Asm::modrmMemSib(U32 reg, U32 base, U32 index, U32 scaleLog2, I32 disp) {
		b((U8)(0x80 | ((reg & 7) << 3) | 4)); // rm=100 => SIB byte follows
		b((U8)(((scaleLog2 & 3) << 6) | ((index & 7) << 3) | (base & 7)));
		d32((U32)disp);
	}

	void
	Asm::memOp(U8 pfx, U8 flags, U8 opcode, U32 reg, Reg base, I32 disp, Reg index, U32 scaleLog2) {
		if(pfx)
			b(pfx);
		B32 w = (flags & kMemW) != 0;
		if(flags & kMemRexForce)
			rexForce(w, reg, index, base);
		else
			rex(w, reg, index, base);
		if(flags & kMemEsc)
			b(0x0f);
		b(opcode);
		if(flags & kMemSib)
			modrmMemSib(reg, base, index, scaleLog2, disp);
		else
			modrmMem(reg, base, disp);
	}

	void Asm::memImmTail(I64 imm, U32 width) {
		if(width == 1)
			b((U8)imm);
		else if(width == 2)
			le::put16(code, (U16)imm);
		else
			d32((U32)(I32)imm);
	}

	U8 Asm::memStoreFlags(U32 width, Reg src) {
		U8 f = width == 8 ? (U8)kMemW : (U8)0;
		if(width == 1 && src >= RSP && src <= RDI)
			f |= (U8)kMemRexForce;
		return f;
	}

	void Asm::movRegImm64(Reg r, U64 imm) {
		rex(true, 0, 0, r);
		b((U8)(0xb8 + (r & 7)));
		d64(imm);
	}

	void Asm::movRR32(Reg dst, Reg src) {
		rex(false, src, 0, dst);
		b(0x89);
		modrmReg(src, dst);
	}

	void Asm::movRR(Reg dst, Reg src) { aluRR(0x89, dst, src); }

	void Asm::movsxd32(Reg dst, Reg src) {
		rex(true, dst, 0, src);
		b(0x63);
		modrmReg(dst, src);
	}

	void Asm::storeMem(Reg base, I32 disp, Reg src, U32 width) {
		memOp(width == 2 ? 0x66 : 0,
					memStoreFlags(width, src),
					width == 1 ? 0x88 : 0x89,
					src,
					base,
					disp);
	}

	void Asm::storeMemSib(Reg base, Reg index, U32 scaleLog2, I32 disp, Reg src, U32 width) {
		memOp(width == 2 ? 0x66 : 0,
					(U8)(memStoreFlags(width, src) | kMemSib),
					width == 1 ? 0x88 : 0x89,
					src,
					base,
					disp,
					index,
					scaleLog2);
	}

	void Asm::storeMemImmSib(Reg base, Reg index, U32 scaleLog2, I32 disp, I64 imm, U32 width) {
		memOp(width == 2 ? 0x66 : 0,
					(U8)((width == 8 ? kMemW : 0) | kMemSib),
					width == 1 ? 0xc6 : 0xc7,
					0,
					base,
					disp,
					index,
					scaleLog2);
		memImmTail(imm, width);
	}

	void Asm::storeMemImm(Reg base, I32 disp, I64 imm, U32 width) {
		memOp(width == 2 ? 0x66 : 0,
					(U8)(width == 8 ? kMemW : 0),
					width == 1 ? 0xc6 : 0xc7,
					0,
					base,
					disp);
		memImmTail(imm, width);
	}

	void Asm::load64(Reg dst, Reg base, I32 disp) { memOp(0, kMemW, 0x8b, dst, base, disp); }

	void Asm::loadExtForm(U32 width, B32 sign, U8& flags, U8& opcode) {
		if(width == 8) {
			flags = kMemW;
			opcode = 0x8b;
		} else if(width == 4) {
			flags = sign ? (U8)kMemW : (U8)0;
			opcode = sign ? 0x63 : 0x8b; // movsxd / mov r32
		} else {											 // movzx/movsx
			flags = kMemW | kMemEsc;
			opcode = width == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7);
		}
	}

	void Asm::loadExt(Reg dst, Reg base, I32 disp, U32 width, B32 sign) {
		U8 flags, opcode;
		loadExtForm(width, sign, flags, opcode);
		memOp(0, flags, opcode, dst, base, disp);
	}

	void Asm::loadExtSib(Reg dst, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width, B32 sign) {
		U8 flags, opcode;
		loadExtForm(width, sign, flags, opcode);
		memOp(0, (U8)(flags | kMemSib), opcode, dst, base, disp, index, scaleLog2);
	}

	void Asm::aluRR(U8 op, Reg dst, Reg src) {
		rex(true, src, 0, dst);
		b(op);
		modrmReg(src, dst);
	}
	void Asm::subRR(Reg d, Reg s) { aluRR(0x29, d, s); }
	void Asm::andRR(Reg d, Reg s) { aluRR(0x21, d, s); }
	void Asm::orRR(Reg d, Reg s) { aluRR(0x09, d, s); }
	void Asm::xorRR(Reg d, Reg s) { aluRR(0x31, d, s); }
	void Asm::cmpRR(Reg d, Reg s) { aluRR(0x39, d, s); }
	void Asm::testRR(Reg d, Reg s) { aluRR(0x85, d, s); }

	void Asm::imulRR(Reg d, Reg s) {
		rex(true, d, 0, s);
		b(0x0f);
		b(0xaf);
		modrmReg(d, s);
	}

	void Asm::aluImm(U8 ext, Reg r, I32 imm) {
		rex(true, 0, 0, r);
		if(imm >= -128 && imm <= 127) { // short imm8 form
			b(0x83);
			modrmReg(ext, r);
			b((U8)imm);
		} else {
			b(0x81);
			modrmReg(ext, r);
			d32((U32)imm);
		}
	}
	void Asm::addRegImm32(Reg r, I32 imm) { aluImm(0, r, imm); }
	void Asm::subRegImm32(Reg r, I32 imm) { aluImm(5, r, imm); }
	void Asm::cmpRegImm32(Reg r, I32 imm) { aluImm(7, r, imm); }

	void Asm::imulRRI(Reg dst, Reg src, I32 imm) {
		rex(true, dst, 0, src);
		if(imm >= -128 && imm <= 127) {
			b(0x6b);
			modrmReg(dst, src);
			b((U8)imm);
		} else {
			b(0x69);
			modrmReg(dst, src);
			d32((U32)imm);
		}
	}

	void Asm::movRegImm32(Reg r, U32 imm) {
		rex(false, 0, 0, r);
		b((U8)(0xb8 + (r & 7)));
		d32(imm);
	}

	void Asm::movRegImmSext32(Reg r, I32 imm) {
		rex(true, 0, 0, r);
		b(0xc7);
		modrmReg(0, r);
		d32((U32)imm);
	}

	void Asm::addRegMem(Reg dst, Reg base, I32 disp) { memOp(0, kMemW, 0x03, dst, base, disp); }

	void Asm::unaryF7W(U8 ext, Reg r, B32 wide) {
		rex(wide, 0, 0, r);
		b(0xf7);
		modrmReg(ext, r);
	}
	void Asm::unaryF7(U8 ext, Reg r) { unaryF7W(ext, r, true); }
	void Asm::negReg(Reg r) { unaryF7(3, r); }
	void Asm::notReg(Reg r) { unaryF7(2, r); }
	void Asm::idivRegW(Reg r, B32 wide) { unaryF7W(7, r, wide); }
	void Asm::divRegW(Reg r, B32 wide) { unaryF7W(6, r, wide); }
	void Asm::cqoW(B32 wide) {
		if(wide)
			rex(true, 0, 0, 0);
		b(0x99);
	}
	void Asm::xorSelf(Reg r) { xorRR(r, r); }

	void Asm::shiftCL(U8 ext, Reg r) {
		rex(true, 0, 0, r);
		b(0xd3);
		modrmReg(ext, r);
	}

	void Asm::rotImm(U8 ext, Reg r, U8 cnt, B32 wide) {
		rex(wide, 0, 0, r);
		b(0xc1);
		modrmReg(ext, r);
		b(cnt);
	}

	void Asm::shiftImm(U8 ext, Reg r, U8 cnt) { rotImm(ext, r, cnt, true); }

	void Asm::setcc(U8 cc, Reg r) {
		if(r >= RSP && r <= RDI)
			rexForce(false, 0, 0, r);
		else
			rex(false, 0, 0, r);
		b(0x0f);
		b((U8)(0x90 + cc));
		b((U8)(0xc0 | (r & 7)));
	}

	void Asm::movzxByte(Reg dst, Reg src) {
		rex(true, dst, 0, src);
		b(0x0f);
		b(0xb6);
		modrmReg(dst, src);
	}

	void Asm::leaMem(Reg dst, Reg base, I32 disp) { memOp(0, kMemW, 0x8d, dst, base, disp); }

	void Asm::leaSib(Reg dst, Reg base, Reg index, U32 scaleLog2, I32 disp) {
		memOp(0, kMemW | kMemSib, 0x8d, dst, base, disp, index, scaleLog2);
	}

	void Asm::leaRipSym(Reg dst, const String& sym, I64 addend) {
		U32 at = leaRipDisp(dst);
		relocs.push_back({at, sym, RelocKind::Pc32, addend - 4});
	}

	void Asm::pushPop(U8 base, Reg r) {
		rex(false, 0, 0, r);
		b((U8)(base + (r & 7)));
	}
	void Asm::push(Reg r) { pushPop(0x50, r); }
	void Asm::pop(Reg r) { pushPop(0x58, r); }
	void Asm::ret() { b(0xc3); }
	void Asm::leave() { b(0xc9); }

	U32 Asm::jmpRel32() {
		b(0xe9);
		U32 at = here();
		d32(0);
		return at;
	}

	U32 Asm::jccRel32(U8 cc) {
		b(0x0f);
		b((U8)(0x80 + cc));
		U32 at = here();
		d32(0);
		return at;
	}

	void Asm::patchRel32(U32 dispAt, U32 target) {
		U32 rel = (U32)((I32)target - (I32)(dispAt + 4));
		for(U32 i = 0; i < 4; ++i)
			code[dispAt + i] = (U8)(rel >> (i * 8));
	}

	void Asm::callSym(const String& sym) {
		b(0xe8);
		U32 at = here();
		relocs.push_back({at, sym, RelocKind::Plt32, -4});
		d32(0);
	}

	U32 Asm::leaRipDisp(Reg dst) {
		rex(true, dst, 0, 0);
		b(0x8d);
		b((U8)(0x05 | ((dst & 7) << 3)));
		U32 at = here();
		d32(0);
		return at;
	}

	void Asm::movsxdSib4(Reg dst, Reg base, Reg index) {
		memOp(0, kMemW | kMemSib, 0x63, dst, base, 0, index, 2);
	}

	void Asm::addRR(Reg dst, Reg src) { aluRR(0x01, dst, src); }

	void Asm::indirectFF(U8 ext, Reg r) {
		rex(false, 0, 0, r);
		b(0xff);
		modrmReg(ext, r);
	}
	void Asm::jmpReg(Reg r) { indirectFF(4, r); }
	void Asm::callReg(Reg r) { indirectFF(2, r); }

	void Asm::probeRsp() {
		b(0x83);
		b(0x0c);
		b(0x24);
		b(0x00);
	}

	U8 Asm::ssePrefixByte(U32 width) { return width == 16 ? 0 : width == 4 ? 0xf3 : 0xf2; }
	void Asm::ssePrefix(U32 width) {
		if(U8 p = ssePrefixByte(width))
			b(p);
	}
	void Asm::movXmm(U8 op, U32 xmm, Reg base, I32 disp, U32 width) {
		memOp(ssePrefixByte(width), kMemEsc, op, xmm, base, disp);
	}
	void Asm::movXmmSib(U8 op, U32 xmm, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width) {
		memOp(ssePrefixByte(width), kMemEsc | kMemSib, op, xmm, base, disp, index, scaleLog2);
	}
	void Asm::loadXmm(U32 xmm, Reg base, I32 disp, U32 width) {
		movXmm(0x10, xmm, base, disp, width);
	}
	void Asm::storeXmm(U32 xmm, Reg base, I32 disp, U32 width) {
		movXmm(0x11, xmm, base, disp, width);
	}
	void Asm::storeXmmSib(U32 xmm, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width) {
		movXmmSib(0x11, xmm, base, index, scaleLog2, disp, width);
	}
	void Asm::loadXmmSib(U32 xmm, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width) {
		movXmmSib(0x10, xmm, base, index, scaleLog2, disp, width);
	}
	void Asm::loadXmmRipSym(U32 xmm, const String& sym, U32 width) {
		ssePrefix(width);
		rex(false, xmm, 0, 0);
		b(0x0f);
		b(0x10);
		b((U8)(0x05 | ((xmm & 7) << 3))); // rip-relative
		U32 at = here();
		relocs.push_back({at, sym, RelocKind::Pc32, -4});
		d32(0);
	}
	void Asm::ssePacked(U8 pfx, U8 op, U32 dst, U32 src, B32 esc38) {
		if(pfx)
			b(pfx);
		rex(false, dst, 0, src);
		b(0x0f);
		if(esc38)
			b(0x38);
		b(op);
		modrmReg(dst, src);
	}
	void Asm::movaps(U32 dst, U32 src) { ssePacked(0, 0x28, dst, src, false); }
	void Asm::sseArith(U8 op, U32 w, U32 d, U32 s) { ssePacked(ssePrefixByte(w), op, d, s, false); }
	void Asm::ucomis(U32 w, U32 a, U32 bx) { ssePacked(w == 8 ? 0x66 : 0, 0x2e, a, bx, false); }
	void Asm::pxor(U32 a, U32 bx) { ssePacked(0x66, 0xef, a, bx, false); }
	void Asm::pcmpeqd(U32 a, U32 bx) { ssePacked(0x66, 0x76, a, bx, false); }
	void Asm::pshufd(U32 dst, U32 src, U8 sel) {
		ssePacked(0x66, 0x70, dst, src, false);
		b(sel);
	}
	void Asm::movdXmmGp(U32 xmm, Reg src, B32 wide) {
		b(0x66);
		rexForce(wide, xmm, 0, src);
		b(0x0f);
		b(0x6e);
		modrmReg(xmm, src);
	}
	void Asm::pinsr(U32 xmm, Reg gp, U8 lane, B32 wide) {
		b(0x66);
		rex(wide, xmm, 0, gp);
		b(0x0f);
		b(0x3a);
		b(0x22);
		modrmReg(xmm, gp);
		b(lane);
	}
	void Asm::movGpXmm(Reg dst, U32 xmm, B32 wide) {
		b(0x66);
		rex(wide, xmm, 0, dst);
		b(0x0f);
		b(0x7e);
		modrmReg(xmm, dst);
	}
	void Asm::sseShiftImm(U32 laneBits, U8 ext, U32 reg, U8 cnt) {
		b(0x66);
		rex(false, 0, 0, reg);
		b(0x0f);
		b(laneBits == 64 ? 0x73 : 0x72);
		modrmReg(ext, reg);
		b(cnt);
	}
	void Asm::cvtRR(U8 pfx, U8 opc, B32 w, U32 dst, U32 src) {
		b(pfx);
		rex(w, dst, 0, src);
		b(0x0f);
		b(opc);
		modrmReg(dst, src);
	}

	void Asm::x87Mem(U8 esc, U8 reg, Reg base, I32 disp) { memOp(0, 0, esc, reg, base, disp); }
	void Asm::fldT(Reg base, I32 disp) { x87Mem(0xdb, 5, base, disp); }
	void Asm::fstpT(Reg base, I32 disp) { x87Mem(0xdb, 7, base, disp); }
	void Asm::fldD(Reg base, I32 disp) { x87Mem(0xd9, 0, base, disp); }
	void Asm::fstpD(Reg base, I32 disp) { x87Mem(0xd9, 3, base, disp); }
	void Asm::fldL(Reg base, I32 disp) { x87Mem(0xdd, 0, base, disp); }
	void Asm::fstpL(Reg base, I32 disp) { x87Mem(0xdd, 3, base, disp); }
	void Asm::fildQ(Reg base, I32 disp) { x87Mem(0xdf, 5, base, disp); }
	void Asm::fistpQ(Reg base, I32 disp) { x87Mem(0xdf, 7, base, disp); }
	void Asm::fnstcw(Reg base, I32 disp) { x87Mem(0xd9, 7, base, disp); }
	void Asm::fldcw(Reg base, I32 disp) { x87Mem(0xd9, 5, base, disp); }

	void Asm::fArithP(U8 enc) {
		b(0xde);
		b(enc);
	}
	void Asm::fchs() {
		b(0xd9);
		b(0xe0);
	}
	void Asm::fucomip() {
		b(0xdf);
		b(0xe9);
	}
	void Asm::fstpReg0() {
		b(0xdd);
		b(0xd8);
	}
} // namespace rat
