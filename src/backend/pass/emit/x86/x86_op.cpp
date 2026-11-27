#include "pass/emit/x86/x86_op.h"

#include <cstdio>

namespace rat {
	namespace detail {
		// clang-format off
		constexpr X86OpInfo kX86OpInfo[] = {
		//mnemonic       transparent
		{"copy",         true},
		{"loadimm",      true},
		{"loadsym",      true},
		{"frameaddr",    true},
		{"retaddr",      true},
		{"lea",          true},
		{"stackalloc",   false},
		{"stacksave",    false},
		{"stackrestore", false},
		{"setjmp",       false},
		{"longjmp",      false},
		{"load",         true},
		{"store",        true},
		{"add",          true},
		{"sub",          true},
		{"mul",          true},
		{"and",          true},
		{"or",           true},
		{"xor",          true},
		{"neg",          true},
		{"not",          true},
		{"shl",          true},
		{"ashr",         true},
		{"lshr",         true},
		{"rotl",         true},
		{"rotr",         true},
		{"sdiv",         false},
		{"srem",         false},
		{"udiv",         false},
		{"urem",         false},
		{"bsf",          true},
		{"bsr",          true},
		{"cmp",          true},
		{"setcc",        true},
		{"cmov",         true},
		{"maskbits",     true},
		{"signextbits",  true},
		{"bswap",        true},
		{"fload",        true},
		{"fstore",       true},
		{"fneg",         true},
		{"fabs",         true},
		{"fcmp",         true},
		{"fcmpflags",    true},
		{"sse",          true},
		{"vshiftimm",    false},
		{"vsplat",       false},
		{"vextract",     false},
		{"x87loadmem",   false},
		{"x87storemem",  false},
		{"x87loadimmd",  false},
		{"x87fromint",   false},
		{"x87toint",     false},
		{"x87fromsse",   false},
		{"x87tosse",     false},
		{"x87add",       false},
		{"x87sub",       false},
		{"x87mul",       false},
		{"x87div",       false},
		{"x87neg",       false},
		{"x87cmp",       false},
		{"call",         true}, // clobbers cover the volatile set, slots are below rsp
		{"ret",          true},
		{"jmp",          true},
		{"switchjump",   true},
		{"br",           true},
		{"vastart",      false},
		{"ud2",          false},
		{"prefetch",     false},
		};
		// clang-format on

		static_assert(std::size(kX86OpInfo) == (U32)X86Op::Prefetch + 1, "one per X86Op");

		// clang-format off
		constexpr const C8* kGpNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
		constexpr const C8* kCcNames[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};
		// clang-format on
		constexpr U32 kMnemonicColumn = 16;

		String operandText(const MachineOperand& operand) {
			switch(operand.kind) {
			case MachineOperand::Kind::VReg:
				return "v" + std::to_string(operand.vreg);
			case MachineOperand::Kind::Phys:
				if(operand.phys >= X86Target::kXmmBase)
					return "xmm" + std::to_string(operand.phys - X86Target::kXmmBase);
				return kGpNames[operand.phys - X86Target::kGpBase];
			case MachineOperand::Kind::Imm:
				return std::to_string(operand.imm);
			case MachineOperand::Kind::FrameSlot:
				return "[rbp" + String(operand.slot < 0 ? "" : "+") + std::to_string(operand.slot) + "]";
			case MachineOperand::Kind::Sym:
				return operand.sym();
			case MachineOperand::Kind::Block:
				return "b" + std::to_string(operand.block);
			default:
				return "_";
			}
		}

		B32 testsCc(const MachineInstr& in) {
			X86Op op = (X86Op)in.op;
			B32 fusedBr = op == X86Op::Br && in.imm2;
			return op == X86Op::SetCC || op == X86Op::CMov || op == X86Op::FCmp || op == X86Op::X87Cmp ||
						 fusedBr;
		}

		// the use holding the index of a memory op, 0 for other ops, see sibBits
		U32 addressIndexUse(X86Op op) {
			if(op == X86Op::Load || op == X86Op::FLoad || op == X86Op::Lea)
				return 1;
			if(op == X86Op::Store || op == X86Op::FStore)
				return 2;
			return 0;
		}

		String addressText(const MachineInstr& in, U32 indexUse) {
			if(in.uses[0].isFrameSlot())
				return operandText(in.uses[0]);
			String text = "[" + operandText(in.uses[0]);
			if(in.imm2 & kSibIndexed) {
				U32 scale = 1u << sibScaleLog2(in.imm2);
				text += " + " + operandText(in.uses[indexUse]) + "*" + std::to_string(scale);
			}
			if(in.imm)
				text += (in.imm < 0 ? " - " : " + ") + std::to_string(std::abs(in.imm));
			return text + "]";
		}

		// pfx, map, opc, w and rex from the low byte up
		I64 packEnc(Enc e, B32 imm8) {
			I64 desc = e.pfx | e.map << 8 | e.opc << 16 | (I64)e.w << 24 | (I64)e.rex << 25;
			return imm8 ? desc | kSseImm8 : desc;
		}

		Enc unpackEnc(I64 desc) {
			return {(U8)desc,
							(U8)(desc >> 8),
							(U8)(desc >> 16),
							(B32)(desc >> 24) & 1,
							(B32)(desc >> 25) & 1};
		}

		// uses[0] = frame slot, uses[1] = source
		B32 isAnySlotStore(const MachineInstr& in) {
			X86Op op = (X86Op)in.op;
			if((op != X86Op::Store && op != X86Op::FStore) || !in.defs.empty() || in.uses.size() != 2)
				return false;
			return in.uses[0].isFrameSlot() && in.uses[1].isPhys();
		}

		U32 blockIdBound(const MachineFunc& mf) {
			I32 maxId = -1;
			for(const MachineBlock& blk : mf.blocks)
				if(blk.id > maxId)
					maxId = blk.id;
			return (U32)(maxId + 1);
		}
	} // namespace detail

	const X86OpInfo& x86OpInfo(X86Op op) { return detail::kX86OpInfo[(U32)op]; }

	// mnemonic, condition, defs, address, uses and the other nonzero immediates. a use repeating the
	// def of a two-address op is left out
	String x86InstrText(const MachineInstr& instr) {
		X86Op op = (X86Op)instr.op;
		List<String> parts;
		I64 imm[] = {instr.imm, instr.imm2};
		if(detail::testsCc(instr)) {
			parts.push_back(detail::kCcNames[instr.imm & 15]);
			imm[0] = 0;
			if(op == X86Op::Br)
				imm[1] = 0;
		}
		for(const MachineOperand& def : instr.defs)
			parts.push_back(detail::operandText(def));
		U32 index = detail::addressIndexUse(op);
		if(index) {
			parts.push_back(detail::addressText(instr, index));
			imm[0] = 0;
			imm[1] = 0;
		}
		B32 tied = !instr.defs.empty() && !instr.uses.empty() &&
							 detail::operandText(instr.defs[0]) == detail::operandText(instr.uses[0]);
		for(U32 i = 0; i < instr.uses.size(); ++i) {
			B32 inAddress = index && (i == 0 || (i == index && (instr.imm2 & detail::kSibIndexed)));
			if(!inAddress && !(i == 0 && tied))
				parts.push_back(detail::operandText(instr.uses[i]));
		}
		if(op == X86Op::Sse) { // the packed Enc, see packEnc
			C8 enc[24];
			std::snprintf(enc, sizeof enc, "#0x%llx", (unsigned long long)imm[0]);
			parts.push_back(enc);
			imm[0] = 0;
		}
		for(I64 v : imm)
			if(v)
				parts.push_back("#" + std::to_string(v));
		String text = x86OpInfo(op).mnemonic;
		if(!parts.empty())
			text.resize(std::max((U32)text.size() + 1, detail::kMnemonicColumn), ' ');
		for(U32 i = 0; i < parts.size(); ++i)
			text += (i ? ", " : "") + parts[i];
		return text;
	}
} // namespace rat
