#ifndef RAT_PASS_EMIT_X86OP_H
#define RAT_PASS_EMIT_X86OP_H

#include "core.h"

#include "codegen/machine_function.h"
#include "target/x86/x86_asm.h"

namespace rat {
	namespace detail {
		constexpr U32 kGp = X86Target::kGpClass;
		constexpr U32 kFp = X86Target::kFpClass;
		constexpr U32 kX87 = X86Target::kX87Class;

		constexpr I64 kX87MemBits = 80;

		constexpr U8 kAluAdd = 0x01;
		constexpr U8 kAluSub = 0x29;
		constexpr U8 kAluAnd = 0x21;
		constexpr U8 kAluOr = 0x09;
		constexpr U8 kAluXor = 0x31;

		constexpr U8 kIntCc[] = {CC_E, CC_NE, CC_L, CC_LE, CC_B, CC_BE};
		// FEq..FGe -> condition code / operand swap (swap keeps lt/le NaN-correct)
		constexpr U8 kFpCc[] = {CC_E, CC_NE, CC_A, CC_AE, CC_A, CC_AE};
		constexpr U8 kFpSwap[] = {0, 0, 1, 1, 0, 0};

		constexpr U8 kSseOp[] = {0x58, 0x5c, 0x59, 0x5e};
	} // namespace detail

	enum class X86Op : U32 {
		// pseudo / data movement
		Copy,			 // dst = src (reg-reg or fill from a fixed phys; RA-coalescable)
		LoadImm,	 // dst = imm
		LoadSym,	 // dst = lea rip[sym]  (address of a global)
		FrameAddr, // dst = lea rbp[imm]  (address of an arbitrary rbp offset; imm = disp)
		Lea,			 // dst = lea [use[0] + use[1]*(1<<imm2) + imm]
		// integer memory: use[0] = address reg, imm = displacement
		Load,	 // dst = [addr + index*scale + disp], sign/zero-extended per width/imm2
		Store, // [addr + index * scale + disp] = src (index in use[2] per imm2), width
		// integer ALU (two-address on the def reg; def and use[0] are coalesced)
		Add,
		Sub,
		Mul,
		And,
		Or,
		Xor,
		Neg,
		Not,
		Shl,
		AShr,
		LShr, // shift count in use[1] (fixed to RCX by lowering)
		Rotl,
		Rotr, // rotate; constant count in use[1], operand width in imm
		SDiv,
		SRem,
		UDiv,
		URem,				 // RAX/RDX/RCX fixed by lowering via Phys operands
		Cmp,				 // flag-setting compare of use[0],use[1]
		SetCC,			 // dst = (cc); condition code in imm
		MaskBits,		 // dst &= ((1<<imm)-1)
		SignExtBits, // dst = sign-extend dst from imm bits to 64
		// SSE scalar float
		FLoad,
		FStore, // [addr+disp] <-> xmm
		FAdd,
		FSub,
		FMul,
		FDiv,			 // two-address on the def xmm
		FNeg,			 // pxor-based scalar negate
		FSqrt,		 // dst = sqrt(use[0]); width in imm
		FAbs,			 // dst = |use[0]| via lane shifts; width in imm
		FCmp,			 // ucomis use[0],use[1] + setcc; cc in imm, swap in imm2
		FCmpFlags, // flags-only ucomis use[0],use[1]; swap in imm2 (cc consumed by a fused Br)
		Cvt,			 // SSE convert; pfx/opc/w packed in imm
		// SSE packed vector (128-bit)
		VArith,		// dst = dst OP use[1] packed; imm = (0f38 escape << 16) | (prefix << 8) | opcode byte
		VSplat,		// dst = broadcast use[0] to all lanes; imm = elem bytes, imm2 = int?
		VExtract, // dst = lane imm of use[0]; imm2 = (elem bytes << 1) | int?
		BitScanF, // dst = bsf use[0], operand width in imm (undefined when use[0] is 0)
		BitScanR, // dst = bsr use[0], operand width in imm (undefined when use[0] is 0)

		VPack, // dst = int/fp lanes use[0..k-1] gathered through the vec scratch slot; imm = elem bytes
		VPackReg, // dst = int lanes use[0..k-1] built in-register via movd/movq + pinsr (sse4.1); imm = elem bytes
		VShuf, // dst = pshufd(use[0], imm)
		// x87 ops
		X87LoadMem,	 // def(slot) = fld [use0 addr]; imm = mem width (4/8/80)
		X87StoreMem, // [use0 addr] = fstp use1(slot); imm = mem width (4/8/80)
		X87LoadImmD, // def(slot) = fld double-bits in use0.imm (via scratch slot)
		X87FromInt,	 // def(slot) = fild use0 (gp), through scratch slot
		X87ToInt,		 // def(gp)   = fistp use0(slot), through scratch slot + cw juggle
		X87FromSse,	 // def(slot) = (long double)use0 (xmm); imm = src width
		X87ToSse,		 // def(xmm)  = (float)use0(slot);   imm = dst width
		X87Add,
		X87Sub,
		X87Mul,
		X87Div, // def(slot) = use0(slot) OP use1(slot)
		X87Neg, // def(slot) = -use0(slot) (fchs)
		X87Cmp, // def(gp) = use0(slot) CMP use1(slot); cc in imm, swap in imm2
		// control / calls
		Call, // direct (sym in sym) or indirect (target reg in use[0]); imm = xmmUsed
		Ret,
		Jmp,				// unconditional, target block in use[0].block
		SwitchJump, // table jump: use[0] = selector reg, use[1..] = slot target blocks
		Br,					// test+jcc: predicate in use[0], then in use[1].block, else in use[2].block
		// variadic support
		VaStart, // init va_list at [use0]; imm = gpOffset start, imm2 = fpOffset start
		VaArg,	 // fetch next vararg into def; use0 = va_list ptr; imm = kind, imm2 = width
	};

	enum class VaArgKind : I64 { Int = 0, Sse = 1, X87 = 2 };

	struct X86FrameLayout : MachineFuncAux {
		I32 ldScratch = 0;
		I32 vecScratch = 0;
		B32 variadic = false;
		I32 saveArea = 0;
		I32 overflowOff = 16;
		U32 namedGp = 0;
		U32 namedFp = 0;
		I32 sretSlot = 0;
	};

	RegAllocHooks x86RegAllocHooks();
} // namespace rat

#endif
