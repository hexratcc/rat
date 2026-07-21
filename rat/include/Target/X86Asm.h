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

	enum class X86VaList : U32 {
		SysV,		 // struct with gp/fp offsets and a register save area
		CharPtr, // plain pointer walking 8-byte home slots (win64)
	};

	struct X86CallConv {
		// argument passing
		const Reg* gpArgs; // integer argument registers, in order
		U32 gpArgCount;
		U32 sseArgCount; // xmm0..n-1
		B32 sharedSlots; // gp/sse arguments draw from one slot sequence (win64)
		B32 x87ByRef;		 // long double passed/returned via hidden pointer (win64)
		// stack frame
		U32 shadowBytes;	 // caller-reserved spill space below outgoing args (win64)
		I32 stackParamOff; // rbp disp of the first incoming stack parameter
		I32 homeOff;			 // rbp disp of the register home area (win64)
		B32 probeStack;		 // touch pages of frames > 4096 to grow the guard page (win64)
		// varargs
		X86VaList vaList;
		B32 alHoldsSseCount; // al = #sse register args at variadic calls (sysv)
		B32 dupSseArgsInGp;	 // mirror sse register args into gp registers (win64)
		U32 gpSaveBytes;		 // sysv va_list register save area geometry
		U32 sseSlotBytes;
		U32 regSaveBytes;
		// registers
		const Reg* gpCalleeSaved;
		U32 gpCalleeSavedCount;
		U32 sseVolatileCount; // xmm0..n-1 volatile
	};

	namespace abi {
		inline constexpr Reg kSysVGpArgs[] = {RDI, RSI, RDX, RCX, R8, R9};
		inline constexpr Reg kSysVGpCalleeSaved[] = {RBX, R12, R13, R14, R15};
		inline constexpr Reg kWin64GpArgs[] = {RCX, RDX, R8, R9};
		inline constexpr Reg kWin64GpCalleeSaved[] = {RBX, RSI, RDI, R12, R13, R14, R15};

		constexpr X86CallConv sysv() {
			X86CallConv c{};
			c.gpArgs = kSysVGpArgs;
			c.gpArgCount = 6;
			c.sseArgCount = 8;
			c.sharedSlots = false;
			c.x87ByRef = false;
			c.shadowBytes = 0;
			c.stackParamOff = 16;
			c.homeOff = 0;
			c.probeStack = false;
			c.vaList = X86VaList::SysV;
			c.alHoldsSseCount = true;
			c.dupSseArgsInGp = false;
			c.gpSaveBytes = 6 * 8;
			c.sseSlotBytes = 16;
			c.regSaveBytes = 6 * 8 + 8 * 16;
			c.gpCalleeSaved = kSysVGpCalleeSaved;
			c.gpCalleeSavedCount = 5;
			c.sseVolatileCount = 16;
			return c;
		}

		constexpr X86CallConv win64() {
			X86CallConv c{};
			c.gpArgs = kWin64GpArgs;
			c.gpArgCount = 4;
			c.sseArgCount = 4;
			c.sharedSlots = true;
			c.x87ByRef = true;
			c.shadowBytes = 32;
			c.stackParamOff = 48;
			c.homeOff = 16;
			c.probeStack = true;
			c.vaList = X86VaList::CharPtr;
			c.alHoldsSseCount = false;
			c.dupSseArgsInGp = true;
			c.gpSaveBytes = 0;
			c.sseSlotBytes = 0;
			c.regSaveBytes = 0;
			c.gpCalleeSaved = kWin64GpCalleeSaved;
			c.gpCalleeSavedCount = 7;
			c.sseVolatileCount = 6;
			return c;
		}

		inline constexpr X86CallConv kSysV = sysv();
		inline constexpr X86CallConv kWin64 = win64();
	} // namespace abi

	inline const X86CallConv& x86CallConv(OS os) {
		return os == OS::Windows ? abi::kWin64 : abi::kSysV;
	}

	struct X86ArgAssigner {
		enum class Kind : U32 {
			Int, // gp register or 8-byte stack slot (also x87-by-ref pointers)
			Sse,
			X87, // by-value 16-byte stack slot (sysv long double)
		};
		struct Loc {
			I32 reg;			// index into the class's argument registers, or -1 for stack
			U32 stackOff; // byte offset among the stack arguments (valid when reg < 0)
		};

		explicit X86ArgAssigner(const X86CallConv& c)
		: conv(c) {}

		Loc next(Kind k) {
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

		const X86CallConv& conv;
		U32 gpUsed = 0, sseUsed = 0; // split-slot counters (sysv)
		U32 slot = 0;								 // shared-slot counter (win64)
		U32 stackBytes = 0;
	};

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

		// mov imm, [base+disp]
		void storeMemImm(Reg base, I32 disp, I64 imm, U32 width) {
			if(width == 2)
				b(0x66);
			rex(width == 8, 0, 0, base);
			b(width == 1 ? 0xc6 : 0xc7);
			modrmMem((Reg)0, base, disp);
			if(width == 1)
				b((U8)imm);
			else if(width == 2) {
				b((U8)imm);
				b((U8)((U16)imm >> 8));
			} else
				d32((U32)(I32)imm);
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

		// movq r64, xmm
		void movqGpXmm(Reg dst, U32 xmm) {
			b(0x66);
			rexForce(true, xmm, 0, dst);
			b(0x0f);
			b(0x7e);
			modrmReg(xmm, dst);
		}

		// or dword [rsp], 0
		void probeRsp() {
			b(0x83);
			b(0x0c);
			b(0x24);
			b(0x00);
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
		void movaps(U32 dst, U32 src) {
			rex(false, dst, 0, src);
			b(0x0f);
			b(0x28);
			modrmReg(dst, src);
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
