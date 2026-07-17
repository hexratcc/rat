#ifndef RAT_TARGET_X86ASM_H
#define RAT_TARGET_X86ASM_H

#include "Core.h"

#include "Target/ObjectFile.h"

namespace rat {
	enum Reg : U8 {
		RAX = 0,
		RCX = 1,
		RDX = 2,
		RBX = 3,
		RSP = 4,
		RBP = 5,
		RSI = 6,
		RDI = 7,
		R8 = 8,
		R9 = 9,
		R10 = 10,
		R11 = 11,
		R12 = 12,
		R13 = 13,
		R14 = 14,
		R15 = 15,
	};

	enum Cc : U8 {
		CC_B = 0x2,
		CC_AE = 0x3,
		CC_E = 0x4,
		CC_NE = 0x5,
		CC_BE = 0x6,
		CC_A = 0x7,
		CC_L = 0xc,
		CC_LE = 0xe,
	};

	namespace sysv {
		constexpr Reg kIntArgRegs[6] = {RDI, RSI, RDX, RCX, R8, R9};
		constexpr U32 kMaxIntArgs = 6;
		constexpr U32 kMaxXmmArgs = 8;
		constexpr U32 kGpSaveBytes = kMaxIntArgs * 8;
		constexpr U32 kXmmSlotBytes = 16;
		constexpr U32 kRegSaveBytes = kGpSaveBytes + kMaxXmmArgs * kXmmSlotBytes;
	} // namespace sysv

	struct AsmReloc {
		U32 offset;
		String symbol;
		RelocKind kind;
		I64 addend;
	};

	struct Asm {
		List<U8>& code;
		List<AsmReloc>& relocs;

		Asm(List<U8>& c, List<AsmReloc>& r)
		: code(c),
			relocs(r) {}

		U32 here() const { return (U32)code.size(); }
		void b(U8 v) { code.push_back(v); }
		void d32(U32 v) {
			for(U32 i = 0; i < 4; ++i)
				b((U8)(v >> (i * 8)));
		}
		void d64(U64 v) {
			for(U32 i = 0; i < 8; ++i)
				b((U8)(v >> (i * 8)));
		}

		static U8 rexByte(B32 w, U32 r, U32 x, U32 rm) {
			return (U8)(0x40 | (w ? 8 : 0) | ((r >> 3) << 2) | ((x >> 3) << 1) | (rm >> 3));
		}

		void rex(B32 w, U32 r, U32 x, U32 rm) {
			U8 v = rexByte(w, r, x, rm);
			if(v != 0x40)
				b(v);
		}

		void rexForce(B32 w, U32 r, U32 x, U32 rm) { b(rexByte(w, r, x, rm)); }

		void modrmReg(U32 reg, U32 rm) { b((U8)(0xc0 | ((reg & 7) << 3) | (rm & 7))); }

		void modrmMem(U32 reg, U32 base, I32 disp) {
			b((U8)(0x80 | ((reg & 7) << 3) | (base & 7)));
			if((base & 7) == 4)
				b(0x24); // SIB: scale=0, index=none, base
			d32((U32)disp);
		}

		// [base + index*(1<<scaleLog2) + disp]
		void modrmMemSib(U32 reg, U32 base, U32 index, U32 scaleLog2, I32 disp) {
			b((U8)(0x80 | ((reg & 7) << 3) | 4)); // rm=100 => SIB byte follows
			b((U8)(((scaleLog2 & 3) << 6) | ((index & 7) << 3) | (base & 7)));
			d32((U32)disp);
		}

		void movRegImm64(Reg r, U64 imm) {
			rex(true, 0, 0, r);
			b((U8)(0xb8 + (r & 7)));
			d64(imm);
		}

		void movRR32(Reg dst, Reg src) {
			rex(false, src, 0, dst);
			b(0x89);
			modrmReg(src, dst);
		}

		void movRR(Reg dst, Reg src) {
			rex(true, src, 0, dst);
			b(0x89);
			modrmReg(src, dst);
		}

		void movsxd32(Reg dst, Reg src) {
			rex(true, dst, 0, src);
			b(0x63);
			modrmReg(dst, src);
		}

		void storeMem(Reg base, I32 disp, Reg src, U32 width) {
			if(width == 2)
				b(0x66);
			if(width == 1 && src >= RSP && src <= RDI)
				rexForce(false, src, 0, base);
			else
				rex(width == 8, src, 0, base);
			b(width == 1 ? 0x88 : 0x89);
			modrmMem(src, base, disp);
		}

		void load64(Reg dst, Reg base, I32 disp) {
			rex(true, dst, 0, base);
			b(0x8b);
			modrmMem(dst, base, disp);
		}

		void loadExt(Reg dst, Reg base, I32 disp, U32 width, B32 sign) {
			if(width == 8) {
				load64(dst, base, disp);
				return;
			}
			if(width == 4) {
				if(sign) {
					rex(true, dst, 0, base);
					b(0x63); // movsxd
					modrmMem(dst, base, disp);
				} else {
					rex(false, dst, 0, base);
					b(0x8b);
					modrmMem(dst, base, disp);
				}
				return;
			}
			// movzx/movsx
			rex(true, dst, 0, base);
			b(0x0f);
			if(width == 1)
				b(sign ? 0xbe : 0xb6);
			else
				b(sign ? 0xbf : 0xb7);
			modrmMem(dst, base, disp);
		}

		void loadExtSib(Reg dst, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width, B32 sign) {
			if(width == 8) {
				rex(true, dst, index, base);
				b(0x8b);
				modrmMemSib(dst, base, index, scaleLog2, disp);
				return;
			}
			if(width == 4) {
				if(sign) {
					rex(true, dst, index, base);
					b(0x63); // movsxd
				} else {
					rex(false, dst, index, base);
					b(0x8b);
				}
				modrmMemSib(dst, base, index, scaleLog2, disp);
				return;
			}
			// movzx/movsx
			rex(true, dst, index, base);
			b(0x0f);
			if(width == 1)
				b(sign ? 0xbe : 0xb6);
			else
				b(sign ? 0xbf : 0xb7);
			modrmMemSib(dst, base, index, scaleLog2, disp);
		}

		void aluRR(U8 op, Reg dst, Reg src) {
			rex(true, src, 0, dst);
			b(op);
			modrmReg(src, dst);
		}
		void subRR(Reg d, Reg s) { aluRR(0x29, d, s); }
		void andRR(Reg d, Reg s) { aluRR(0x21, d, s); }
		void orRR(Reg d, Reg s) { aluRR(0x09, d, s); }
		void xorRR(Reg d, Reg s) { aluRR(0x31, d, s); }
		void cmpRR(Reg d, Reg s) { aluRR(0x39, d, s); }
		void testRR(Reg d, Reg s) { aluRR(0x85, d, s); }

		void imulRR(Reg d, Reg s) {
			rex(true, d, 0, s);
			b(0x0f);
			b(0xaf);
			modrmReg(d, s);
		}

		void groupImm32(U8 ext, Reg r, I32 imm) {
			rex(true, 0, 0, r);
			b(0x81);
			modrmReg(ext, r);
			d32((U32)imm);
		}
		void groupImm8(U8 ext, Reg r, I8 imm) {
			rex(true, 0, 0, r);
			b(0x83);
			modrmReg(ext, r);
			b((U8)imm);
		}
		// group-1 ALU op with an immediate, picking the short imm8 form when it fits
		void aluImm(U8 ext, Reg r, I32 imm) {
			if(imm >= -128 && imm <= 127)
				groupImm8(ext, r, (I8)imm);
			else
				groupImm32(ext, r, imm);
		}
		void addRegImm32(Reg r, I32 imm) { aluImm(0, r, imm); }
		void subRegImm32(Reg r, I32 imm) { aluImm(5, r, imm); }
		void cmpRegImm32(Reg r, I32 imm) { aluImm(7, r, imm); }

		// dst = src * imm
		void imulRRI(Reg dst, Reg src, I32 imm) {
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

		// mov r32, imm32
		void movRegImm32(Reg r, U32 imm) {
			rex(false, 0, 0, r);
			b((U8)(0xb8 + (r & 7)));
			d32(imm);
		}

		// mov r64, imm32
		void movRegImmSext32(Reg r, I32 imm) {
			rex(true, 0, 0, r);
			b(0xc7);
			modrmReg(0, r);
			d32((U32)imm);
		}

		// dst += [base + disp]  (64-bit)
		void addRegMem(Reg dst, Reg base, I32 disp) {
			rex(true, dst, 0, base);
			b(0x03);
			modrmMem(dst, base, disp);
		}

		void unaryF7(U8 ext, Reg r) {
			rex(true, 0, 0, r);
			b(0xf7);
			modrmReg(ext, r);
		}
		void negReg(Reg r) { unaryF7(3, r); }
		void notReg(Reg r) { unaryF7(2, r); }
		void unaryF7W(U8 ext, Reg r, B32 wide) {
			rex(wide, 0, 0, r);
			b(0xf7);
			modrmReg(ext, r);
		}
		void idivRegW(Reg r, B32 wide) { unaryF7W(7, r, wide); }
		void divRegW(Reg r, B32 wide) { unaryF7W(6, r, wide); }
		void cqoW(B32 wide) {
			if(wide)
				rex(true, 0, 0, 0);
			b(0x99);
		}
		void xorSelf(Reg r) { xorRR(r, r); }

		void shiftCL(U8 ext, Reg r) {
			rex(true, 0, 0, r);
			b(0xd3);
			modrmReg(ext, r);
		}

		void shiftImm(U8 ext, Reg r, U8 cnt) {
			rex(true, 0, 0, r);
			b(0xc1);
			modrmReg(ext, r);
			b(cnt);
		}

		void setcc(U8 cc, Reg r) {
			if(r >= RSP && r <= RDI)
				rexForce(false, 0, 0, r);
			else
				rex(false, 0, 0, r);
			b(0x0f);
			b((U8)(0x90 + cc));
			b((U8)(0xc0 | (r & 7)));
		}

		void movzxByte(Reg dst, Reg src) {
			rex(true, dst, 0, src);
			b(0x0f);
			b(0xb6);
			modrmReg(dst, src);
		}

		void leaMem(Reg dst, Reg base, I32 disp) {
			rex(true, dst, 0, base);
			b(0x8d);
			modrmMem(dst, base, disp);
		}

		// lea dst, [base + index*(1<<scaleLog2) + disp]
		void leaSib(Reg dst, Reg base, Reg index, U32 scaleLog2, I32 disp) {
			rex(true, dst, index, base);
			b(0x8d);
			modrmMemSib(dst, base, index, scaleLog2, disp);
		}

		void leaRipSym(Reg dst, const String& sym, I64 addend) {
			rex(true, dst, 0, 0);
			b(0x8d);
			b((U8)(0x05 | ((dst & 7) << 3))); // rip-relative
			U32 at = here();
			relocs.push_back({at, sym, RelocKind::Pc32, addend - 4});
			d32(0);
		}

		void push(Reg r) {
			if(r >= R8)
				b(0x41);
			b((U8)(0x50 + (r & 7)));
		}
		void ret() { b(0xc3); }
		void leave() { b(0xc9); }

		U32 jmpRel32() {
			b(0xe9);
			U32 at = here();
			d32(0);
			return at;
		}

		U32 jccRel32(U8 cc) {
			b(0x0f);
			b((U8)(0x80 + cc));
			U32 at = here();
			d32(0);
			return at;
		}

		void patchRel32(U32 dispAt, U32 target) {
			U32 rel = (U32)((I32)target - (I32)(dispAt + 4));
			for(U32 i = 0; i < 4; ++i)
				code[dispAt + i] = (U8)(rel >> (i * 8));
		}

		void callSym(const String& sym) {
			b(0xe8);
			U32 at = here();
			relocs.push_back({at, sym, RelocKind::Plt32, -4});
			d32(0);
		}

		void callReg(Reg r) {
			if(r >= R8)
				b(0x41);
			b(0xff);
			modrmReg(2, r);
		}

		static U8 ssePrefixByte(U32 width) { return width == 4 ? 0xf3 : 0xf2; }
		void ssePrefix(U32 width) { b(ssePrefixByte(width)); }
		void movXmm(U8 op, U32 xmm, Reg base, I32 disp, U32 width) {
			ssePrefix(width);
			rex(false, xmm, 0, base);
			b(0x0f);
			b(op);
			modrmMem(xmm, base, disp);
		}
		void loadXmm(U32 xmm, Reg base, I32 disp, U32 width) { movXmm(0x10, xmm, base, disp, width); }
		void storeXmm(U32 xmm, Reg base, I32 disp, U32 width) { movXmm(0x11, xmm, base, disp, width); }
		void loadXmmSib(U32 xmm, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width) {
			ssePrefix(width);
			rex(false, xmm, index, base);
			b(0x0f);
			b(0x10); // movss/movsd load
			modrmMemSib(xmm, base, index, scaleLog2, disp);
		}
		void sseArith(U8 op, U32 width, U32 dst, U32 src) {
			ssePrefix(width);
			rex(false, dst, 0, src);
			b(0x0f);
			b(op);
			modrmReg(dst, src);
		}
		void ucomis(U32 width, U32 a, U32 bx) {
			if(width == 8)
				b(0x66);
			rex(false, a, 0, bx);
			b(0x0f);
			b(0x2e);
			modrmReg(a, bx);
		}
		void pxor(U32 a, U32 bx) {
			b(0x66);
			rex(false, a, 0, bx);
			b(0x0f);
			b(0xef);
			modrmReg(a, bx);
		}
		void cvtRR(U8 pfx, U8 opc, B32 w, U32 dst, U32 src) {
			b(pfx);
			rex(w, dst, 0, src);
			b(0x0f);
			b(opc);
			modrmReg(dst, src);
		}

		void x87Mem(U8 esc, U8 reg, Reg base, I32 disp) {
			rex(false, 0, 0, base);
			b(esc);
			modrmMem(reg, base, disp);
		}
		void fldT(Reg base, I32 disp) { x87Mem(0xdb, 5, base, disp); }
		void fstpT(Reg base, I32 disp) { x87Mem(0xdb, 7, base, disp); }
		void fldD(Reg base, I32 disp) { x87Mem(0xd9, 0, base, disp); }
		void fstpD(Reg base, I32 disp) { x87Mem(0xd9, 3, base, disp); }
		void fldL(Reg base, I32 disp) { x87Mem(0xdd, 0, base, disp); }
		void fstpL(Reg base, I32 disp) { x87Mem(0xdd, 3, base, disp); }
		void fildQ(Reg base, I32 disp) { x87Mem(0xdf, 5, base, disp); }
		void fistpQ(Reg base, I32 disp) { x87Mem(0xdf, 7, base, disp); }
		void fnstcw(Reg base, I32 disp) { x87Mem(0xd9, 7, base, disp); }
		void fldcw(Reg base, I32 disp) { x87Mem(0xd9, 5, base, disp); }

		void faddp() {
			b(0xde);
			b(0xc1);
		}
		void fsubp() {
			b(0xde);
			b(0xe9);
		}
		void fmulp() {
			b(0xde);
			b(0xc9);
		}
		void fdivp() {
			b(0xde);
			b(0xf9);
		}
		void fchs() {
			b(0xd9);
			b(0xe0);
		}
		void fucomip() {
			b(0xdf);
			b(0xe9);
		}
		void fstpReg0() {
			b(0xdd);
			b(0xd8);
		}
	};
} // namespace rat

#endif
