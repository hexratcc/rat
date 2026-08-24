#ifndef RAT_TARGET_X86ASM_H
#define RAT_TARGET_X86ASM_H

#include "core.h"

#include "target/object_file.h"

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
		CC_P = 0xa,
		CC_NP = 0xb,
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

	const X86CallConv& x86CallConv(OS os);

	// assigns each argument to a register index or a stack offset, in call order
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

		Loc next(Kind k);

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

	// x86-64 instruction encoder: appends machine bytes to code and reloc records to relocs.
	// definitions in x86_asm.cpp
	struct Asm {
		List<U8>& code;
		List<AsmReloc>& relocs;

		Asm(List<U8>& c, List<AsmReloc>& r)
		: code(c),
			relocs(r) {}

		// byte-stream primitives
		U32 here() const;
		void b(U8 v);
		void d32(U32 v);
		void d64(U64 v);

		// prefixes and modrm
		static U8 rexByte(B32 w, U32 r, U32 x, U32 rm);
		void rex(B32 w, U32 r, U32 x, U32 rm); // omit the prefix when it would be a bare 0x40
		void rexForce(B32 w, U32 r, U32 x, U32 rm);
		void modrmReg(U32 reg, U32 rm);
		void modrmMem(U32 reg, U32 base, I32 disp);
		// [base + index*(1<<scaleLog2) + disp]
		void modrmMemSib(U32 reg, U32 base, U32 index, U32 scaleLog2, I32 disp);

		enum MemFlags : U8 {
			kMemW = 1,				// REX.W
			kMemRexForce = 2, // emit REX even when it would be a bare 0x40
			kMemEsc = 4,			// 0x0f two-byte opcode escape
			kMemSib = 8,			// [base + index * scale + disp] instead of [base + disp]
		};

		// generic reg-mem opcode with optional prefix/escape/SIB (flags from MemFlags)
		void memOp(U8 pfx,
							 U8 flags,
							 U8 opcode,
							 U32 reg,
							 Reg base,
							 I32 disp,
							 Reg index = (Reg)0,
							 U32 scaleLog2 = 0);
		void memImmTail(I64 imm, U32 width);
		static U8 memStoreFlags(U32 width, Reg src);

		void movRegImm64(Reg r, U64 imm);
		void movRR32(Reg dst, Reg src);
		void movRR(Reg dst, Reg src);
		void movsxd32(Reg dst, Reg src);

		void storeMem(Reg base, I32 disp, Reg src, U32 width);
		void storeMemSib(Reg base, Reg index, U32 scaleLog2, I32 disp, Reg src, U32 width);
		void storeMemImmSib(Reg base, Reg index, U32 scaleLog2, I32 disp, I64 imm, U32 width);
		// mov imm, [base+disp]
		void storeMemImm(Reg base, I32 disp, I64 imm, U32 width);

		void load64(Reg dst, Reg base, I32 disp);
		// REX.W / opcode (and the 0x0f escape) for a width-and-sign extending load
		static void loadExtForm(U32 width, B32 sign, U8& flags, U8& opcode);
		void loadExt(Reg dst, Reg base, I32 disp, U32 width, B32 sign);
		void loadExtSib(Reg dst, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width, B32 sign);

		void aluRR(U8 op, Reg dst, Reg src);
		void subRR(Reg d, Reg s);
		void andRR(Reg d, Reg s);
		void orRR(Reg d, Reg s);
		void xorRR(Reg d, Reg s);
		void cmpRR(Reg d, Reg s);
		void testRR(Reg d, Reg s);
		void imulRR(Reg d, Reg s);

		// group-1 ALU op with an immediate, picking the short imm8 form when it fits
		void aluImm(U8 ext, Reg r, I32 imm);
		void addRegImm32(Reg r, I32 imm);
		void subRegImm32(Reg r, I32 imm);
		void cmpRegImm32(Reg r, I32 imm);
		// dst = src * imm
		void imulRRI(Reg dst, Reg src, I32 imm);

		// mov r32, imm32
		void movRegImm32(Reg r, U32 imm);
		// mov r64, imm32
		void movRegImmSext32(Reg r, I32 imm);
		// dst += [base + disp]  (64-bit)
		void addRegMem(Reg dst, Reg base, I32 disp);

		void unaryF7W(U8 ext, Reg r, B32 wide);
		void unaryF7(U8 ext, Reg r);
		void negReg(Reg r);
		void notReg(Reg r);
		void idivRegW(Reg r, B32 wide);
		void divRegW(Reg r, B32 wide);
		void cqoW(B32 wide);
		void xorSelf(Reg r);

		void shiftCL(U8 ext, Reg r);
		// C1 /ext ib rotate/shift with explicit operand width (ext 0 = rol, 1 = ror)
		void rotImm(U8 ext, Reg r, U8 cnt, B32 wide);
		void shiftImm(U8 ext, Reg r, U8 cnt);

		void setcc(U8 cc, Reg r);
		void movzxByte(Reg dst, Reg src);
		void bitScan(B32 reverse, Reg dst, Reg src, B32 wide);
		void bswap(Reg r, B32 wide);
		void prefetch(U8 hint, Reg base, I32 disp);
		void ud2();

		void leaMem(Reg dst, Reg base, I32 disp);
		// lea dst, [base + index*(1<<scaleLog2) + disp]
		void leaSib(Reg dst, Reg base, Reg index, U32 scaleLog2, I32 disp);
		void leaRipSym(Reg dst, const String& sym, I64 addend);

		void pushPop(U8 base, Reg r);
		void push(Reg r);
		void pop(Reg r);
		void ret();
		void leave();

		U32 jmpRel32();
		U32 jccRel32(U8 cc);
		void patchRel32(U32 dispAt, U32 target);
		void callSym(const String& sym);
		// lea dst, [rip+disp32]; returns the disp offset for later patching
		U32 leaRipDisp(Reg dst);

		// movsxd dst, dword [base + index*4]
		void movsxdSib4(Reg dst, Reg base, Reg index);
		void addRR(Reg dst, Reg src);
		// 0xff /ext indirect through a register (jmp=4, call=2)
		void indirectFF(U8 ext, Reg r);
		void jmpReg(Reg r);
		void callReg(Reg r);
		// or dword [rsp], 0
		void probeRsp();

		static U8 ssePrefixByte(U32 width);
		void ssePrefix(U32 width);
		void movXmm(U8 op, U32 xmm, Reg base, I32 disp, U32 width);
		// movss/movsd, op 0x10 = load, 0x11 = store
		void movXmmSib(U8 op, U32 xmm, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width);
		void loadXmm(U32 xmm, Reg base, I32 disp, U32 width);
		void storeXmm(U32 xmm, Reg base, I32 disp, U32 width);
		void storeXmmSib(U32 xmm, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width);
		void loadXmmSib(U32 xmm, Reg base, Reg index, U32 scaleLog2, I32 disp, U32 width);
		void loadXmmRipSym(U32 xmm, const String& sym, U32 width);
		// packed SSE op with an explicit mandatory prefix (0 = none), esc38 adds the sse4.1 escape
		void ssePacked(U8 pfx, U8 op, U32 dst, U32 src, B32 esc38);
		void movaps(U32 dst, U32 src);
		void sseArith(U8 op, U32 w, U32 d, U32 s);
		void ucomis(U32 w, U32 a, U32 bx);
		void pxor(U32 a, U32 bx);
		// pcmpeqd a, a builds an all-ones register
		void pcmpeqd(U32 a, U32 bx);
		// pshufd dst, src, sel
		void pshufd(U32 dst, U32 src, U8 sel);
		// movd/movq xmm, r32/r64
		void movdXmmGp(U32 xmm, Reg src, B32 wide);
		// pinsrd/pinsrq: insert a gp value into xmm lane (sse4.1); wide selects pinsrq
		void pinsr(U32 xmm, Reg gp, U8 lane, B32 wide);
		// movd/movq r32/r64, xmm (wide selects movq)
		void movGpXmm(Reg dst, U32 xmm, B32 wide);
		void sseShiftImm(U32 laneBits, U8 ext, U32 reg, U8 cnt);
		void cvtRR(U8 pfx, U8 opc, B32 w, U32 dst, U32 src);

		void x87Mem(U8 esc, U8 reg, Reg base, I32 disp);
		void fldT(Reg base, I32 disp);
		void fstpT(Reg base, I32 disp);
		void fldD(Reg base, I32 disp);
		void fstpD(Reg base, I32 disp);
		void fldL(Reg base, I32 disp);
		void fstpL(Reg base, I32 disp);
		void fildQ(Reg base, I32 disp);
		void fistpQ(Reg base, I32 disp);
		void fnstcw(Reg base, I32 disp);
		void fldcw(Reg base, I32 disp);
		void fArithP(U8 enc);
		void fchs();
		void fucomip();
		void fstpReg0();
	};
} // namespace rat

#endif
