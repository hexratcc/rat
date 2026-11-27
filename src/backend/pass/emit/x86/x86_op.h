#ifndef RAT_PASS_EMIT_X86OP_H
#define RAT_PASS_EMIT_X86OP_H

#include "core.h"

#include "codegen/machine_function.h"
#include "ir/node.h"
#include "target/x86/x86_asm.h"

namespace rat {
	namespace detail {
		constexpr U32 kGp = X86Target::kGpClass;
		constexpr U32 kFp = X86Target::kFpClass;
		constexpr U32 kX87 = X86Target::kX87Class;

		inline PhysReg gpPhys(Reg r) { return X86Target::kGpBase + (PhysReg)r; }
		inline PhysReg xmmPhys(U32 n) { return X86Target::kXmmBase + n; }
		inline U64 physBit(PhysReg p) { return (U64)1 << p; }
		inline U64 scratchBits() { return physBit(gpPhys(R10)) | physBit(gpPhys(R11)); }

		constexpr I64 kX87MemBits = 80;

		constexpr I32 kNoSlot = INT32_MIN;
		// rbp sits this far below the cfa, past the return address and saved rbp
		constexpr I32 kCfaFromRbp = 16;
		// dwarf register number per PhysReg, xmm already matches theirs
		constexpr U32 kDwarfRegs[] = {0,	0,	2,	1,	3,	7,	6,	4,	5,	8,	9,
																	10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21,
																	22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
		static_assert(std::size(kDwarfRegs) == X86Target::kRegEnd, "one per PhysReg");

		// Sse imm bit, set when imm2 holds an imm8 that follows the modrm
		constexpr I64 kSseImm8 = (I64)1 << 26;

		// imm2 bits of the memory ops, see sibBits
		constexpr I64 kSibSign = 1;
		constexpr I64 kSibIndexed = 2;
		constexpr U32 sibScaleLog2(I64 imm2) { return (U32)(imm2 >> 2 & 3); }
	} // namespace detail

	enum class X86Op : U32 {
		// pseudo / data movement
		Copy,			 // dst = src (reg-reg or fill from a fixed phys, RA-coalescable)
		LoadImm,	 // dst = imm
		LoadSym,	 // dst = lea rip[sym]  (address of a global)
		FrameAddr, // dst = lea rbp[imm]  (address of an arbitrary rbp offset, imm = disp)
		RetAddr,	 // dst = [rbp+8]      (this frame's return address, forces a frame)
		Lea,			 // dst = lea [use[0] + use[1]*scale + imm], imm2 as for Load
		// dynamic stack
		StackAlloc,		// rsp -= roundup(use[0], 16), rsp &= ~15, dst = rsp
		StackSave,		// dst = rsp
		StackRestore, // rsp = use[0]
		// non-local goto, use[0] points at [frame pointer, resume address, stack pointer]
		SetJmp,	 // fills the buffer, def = 0 falling through, 1 when a LongJmp lands
		LongJmp, // restores rbp/rsp from the buffer and jumps to the resume address
		// integer memory, use[0] = address reg or an 8-byte frame slot, imm = displacement
		// imm2 = (scaleLog2 << 2) | (indexed << 1) | sign, see sibBits
		Load,	 // dst = [addr + index*scale + disp], index in use[1], sign or zero extended
		Store, // [addr + index*scale + disp] = use[1], index in use[2], width of use[1]
		// integer ALU (two-address on the def reg, def and use[0] are coalesced)
		Add,
		Sub,
		Mul,
		And,
		Or,
		Xor,
		Neg,
		Not,
		// shift count in use[1], an imm or RCX
		Shl,
		AShr,
		LShr,
		// rotate, constant count in use[1], operand width in imm
		Rotl,
		Rotr,
		// RAX/RDX/RCX fixed by lowering via Phys operands
		SDiv,
		SRem,
		UDiv,
		URem,
		// bit ops, flags and extensions
		BitScanF,		 // dst = bsf use[0], operand width in imm (undefined when use[0] is 0)
		BitScanR,		 // dst = bsr use[0], operand width in imm (undefined when use[0] is 0)
		Cmp,				 // flag-setting compare of use[0],use[1]
		SetCC,			 // dst = (cc), condition code in imm
		CMov,				 // dst = (cc) ? use[1] : dst
		MaskBits,		 // dst &= ((1<<imm)-1)
		SignExtBits, // dst = sign-extend dst from imm bits to 64
		Bswap,			 // dst = byte-reverse dst, operand width in imm (32 or 64)
		// SSE scalar float
		FLoad,
		FStore,		 // [addr+disp] <-> xmm
		FNeg,			 // pxor-based negate, lane width in imm
		FAbs,			 // dst = |use[0]| via lane shifts, width in imm
		FCmp,			 // ucomis use[0],use[1] + setcc, cc in imm, swap in imm2
		FCmpFlags, // flags-only ucomis use[0],use[1], swap in imm2 (cc consumed by a fused Br)
		// any sse instr
		// dst = OP use[1] or OP use[0] when it is the only use, the Enc packed in imm
		// plus kSseImm8, see there. A use[2] of xmm0 is the implicit blendv mask
		Sse,
		// SSE packed vector (128-bit)
		VShiftImm, // dst = dst shifted by an immediate, imm = lane bits, imm2 = (/ext << 8) | count
		VSplat,		 // dst = broadcast use[0] to all lanes, imm = elem bytes, imm2 = int?
		VExtract,	 // dst = lane imm of use[0], imm2 = (elem bytes << 1) | int?
		// x87 ops
		// imm 80 moves an m80 at [use0 addr], imm -1 and -2 work on st0
		X87LoadMem,	 // def(slot) = fld [use0 addr], imm -1 pushes use0(slot) to st0
		X87StoreMem, // [use0 addr] = fstp use1(slot), imm -1 pops st0 to def(slot), imm -2 drops it
		X87LoadImmD, // def(slot) = fld double-bits in use0.imm (via scratch slot)
		X87FromInt,	 // def(slot) = fild use0 (gp), through scratch slot
		X87ToInt,		 // def(gp)   = fistp use0(slot), through scratch slot + cw juggle
		X87FromSse,	 // def(slot) = (long double)use0 (xmm), imm = src width, imm 80 copies slot use0
		X87ToSse,		 // def(xmm)  = (float)use0(slot),   imm = dst width
		// def(slot) = use0(slot) OP use1(slot)
		X87Add,
		X87Sub,
		X87Mul,
		X87Div,
		X87Neg, // def(slot) = -use0(slot) (fchs)
		X87Cmp, // def(gp) = use0(slot) CMP use1(slot), cc in imm, swap in imm2
		// control / calls
		Call, // to the sym use, or through the r11 use when imm2 is set, imm = stack arg bytes
		Ret,
		Jmp,				// unconditional, target block in use[0].block
		SwitchJump, // table jump, use[0] = selector reg, use[1..] = slot target blocks
		Br,					// jnz use[0] to use[1] else use[2], with imm2 set jcc imm to use[0] else use[1]
		// variadic support
		VaStart, // init va_list at [use0], imm = gpOffset start, imm2 = fpOffset start
		// misc
		Ud2,			// undefined instruction
		Prefetch, // prefetch [use0 + imm], imm2 = hint (0 = nta, 1 = t0, 2 = t1, 3 = t2)
	};

	struct X86OpInfo {
		const C8* mnemonic;
		B32 transparent; // reaches no frame memory outside its operands and is not re-entered
	};

	const X86OpInfo& x86OpInfo(X86Op op);

	namespace detail {
		I64 packEnc(Enc e, B32 imm8);
		Enc unpackEnc(I64 desc);
		String operandText(const MachineOperand& operand);
		B32 testsCc(const MachineInstr& in);
		U32 addressIndexUse(X86Op op);
		String addressText(const MachineInstr& in, U32 indexUse);
		B32 isAnySlotStore(const MachineInstr& in);
		U32 blockIdBound(const MachineFunc& mf);
	} // namespace detail

	String x86InstrText(const MachineInstr& instr);

	struct X86FrameLayout : MachineFuncAux {
		I32 ldScratch = 0;
		I32 vecScratch = 0;
		B32 variadic = false;
		I32 saveArea = 0;
		I32 overflowOff = 16;
		U32 namedGp = 0;
		U32 namedFp = 0;
		I32 sretSlot = 0;
		List<I32> allocOff; // node id -> alloca frame offset, kNoSlot none
	};
} // namespace rat

#endif
