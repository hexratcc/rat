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
			c.stackParamOff = 16;
			c.vaList = X86VaList::SysV;
			c.alHoldsSseCount = true;
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
			c.dupSseArgsInGp = true;
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
		Loc nextMem(U32 bytes, U32 align);

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

	// [base + disp], or [base + index*(1<<scaleLog2) + disp] when indexed
	struct Mem {
		Reg base;
		I32 disp = 0;
		Reg index = (Reg)0;
		U32 scaleLog2 = 0;
		B32 indexed = false;
	};

	// the only way to build an indexed Mem, a bare index without indexed would encode wrong
	Mem indexedMem(Reg base, Reg index, U32 scaleLog2, I32 disp);

	// an opcode with its mandatory prefix and escape bytes
	struct Enc {
		U8 pfx = 0; // 0x66, 0xf2, 0xf3 or none
		U8 map = 0; // 0x0f, 0x38 for 0f 38, 0x3a for 0f 3a, or none
		U8 opc = 0;
		B32 w = 0;	 // REX.W
		B32 rex = 0; // a bare 0x40 rex too, spl..dil need it as byte operands
	};

	// x86-64 instruction encoder, appends machine bytes to code and reloc records to relocs.
	// reg is the modrm reg field, a register or the /digit of the opcode
	struct Asm {
		List<U8>& code;
		List<AsmReloc>& relocs;

		Asm(List<U8>& c, List<AsmReloc>& r)
		: code(c),
			relocs(r) {}

		U32 here() const;
		void b(U8 v);
		void d32(U32 v);
		void d64(U64 v);
		void imm(I64 v, U32 bytes);
		// multi-byte nops up to the next multiple of a
		void alignNop(U32 a);

		void rr(Enc e, U32 reg, U32 rm);
		void rm(Enc e, U32 reg, const Mem& mem);
		// [rip + disp32], returns the offset of disp32
		U32 rip(Enc e, U32 reg);
		void ripSym(Enc e, U32 reg, const String& sym);
		// the register in the low opcode bits
		void opReg(Enc e, U32 r);
		// the imm8 opcode when imm fits, else the imm32 one
		void rrImm(U8 op8, U8 op32, U32 reg, U32 rm, I32 imm);
		// group-1 alu op on a 64-bit register
		void aluImm(U8 ext, Reg r, I32 imm);

		// width-sized moves
		void movImm64(Reg r, U64 imm);
		void load(Reg dst, const Mem& mem, U32 width, B32 sign);
		void store(const Mem& mem, Reg src, U32 width);
		void storeImm(const Mem& mem, I64 imm, U32 width);
		void loadXmm(U32 xmm, const Mem& mem, U32 width);
		void storeXmm(U32 xmm, const Mem& mem, U32 width);
		static U8 ssePrefixByte(U32 width);

		U32 jmpRel32();
		U32 jccRel32(U8 cc);
		void patchRel32(U32 dispAt, U32 target);
		void callSym(const String& sym);
	private:
		U32 reserve32();
		void lead(Enc e, U32 reg, U32 index, U32 base);
		void modrmMem(U32 reg, const Mem& mem);
	};

	// common encodings, W marks the 64-bit form
	namespace enc {
		constexpr Enc kMovW{0, 0, 0x89, 1}; // mov r/m, r
		constexpr Enc kLeaW{0, 0, 0x8d, 1};
		constexpr Enc kMovsxdW{0, 0, 0x63, 1}; // movsxd r, r/m32
		constexpr Enc kTestW{0, 0, 0x85, 1};
		constexpr Enc kF7W{0, 0, 0xf7, 1}; // group 3, /2 not /3 neg /6 div /7 idiv
		constexpr Enc kC1W{0, 0, 0xc1, 1}; // group 2 by imm8, /4 shl /5 shr /7 sar
		constexpr Enc kPush{0, 0, 0x50};
		constexpr Enc kPop{0, 0, 0x58};
		constexpr Enc kMovImm32{0, 0, 0xb8};
		constexpr Enc kFf{0, 0, 0xff}; // group 5, /2 call /4 jmp
		constexpr Enc kMovaps{0, 0x0f, 0x28};
		constexpr Enc kMovdToXmm{0x66, 0x0f, 0x6e, 0, 1};
		constexpr Enc kMovdFromXmm{0x66, 0x0f, 0x7e};
		constexpr Enc kPshufd{0x66, 0x0f, 0x70};
		constexpr Enc kPxor{0x66, 0x0f, 0xef};
		constexpr Enc kSseShift{0x66, 0x0f, 0x72}; // dword lanes by imm8, 0x73 for qword lanes
	} // namespace enc
} // namespace rat

#endif
