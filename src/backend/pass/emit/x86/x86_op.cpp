#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		// clang-format off
		const X86OpInfo kX86OpInfo[] = {
		//mnemonic       flags
		// pseudo / data movement
		{"copy",         kX86Transparent | kX86KeepsLowBits},
		{"loadimm",      kX86Transparent},
		{"loadsym",      kX86Transparent},
		{"frameaddr",    kX86Transparent},
		{"retaddr",      kX86Transparent},
		{"lea",          kX86Transparent | kX86KeepsLowBits},
		// dynamic stack
		{"stackalloc",   0},
		{"stacksave",    0},
		{"stackrestore", 0},
		// non-local goto
		{"setjmp",       0},
		{"longjmp",      0},
		// integer memory
		{"load",         kX86Transparent | kX86KeepsLowBits},
		{"store",        kX86Transparent},
		// integer ALU
		{"add",          kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"sub",          kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"mul",          kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"and",          kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"or",           kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"xor",          kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"neg",          kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"not",          kX86Transparent | kX86KeepsLowBits},
		{"shl",          kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"ashr",         kX86Transparent | kX86WritesFlags},
		{"lshr",         kX86Transparent | kX86WritesFlags},
		{"rotl",         kX86Transparent | kX86WritesFlags},
		{"rotr",         kX86Transparent | kX86WritesFlags},
		{"sdiv",         kX86WritesFlags},
		{"srem",         kX86WritesFlags},
		{"udiv",         kX86WritesFlags},
		{"urem",         kX86WritesFlags},
		{"bsf",          kX86Transparent | kX86WritesFlags},
		{"bsr",          kX86Transparent | kX86WritesFlags},
		{"cmp",          kX86Transparent | kX86WritesFlags},
		{"setcc",        kX86Transparent | kX86ReadsFlags},
		{"cmov",         kX86Transparent | kX86ReadsFlags | kX86KeepsLowBits},
		{"maskbits",     kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"signextbits",  kX86Transparent | kX86KeepsLowBits},
		{"bswap",        kX86Transparent},
		// sse scalar float
		{"fload",        kX86Transparent},
		{"fstore",       kX86Transparent},
		{"fadd",         kX86Transparent},
		{"fsub",         kX86Transparent},
		{"fmul",         kX86Transparent},
		{"fdiv",         kX86Transparent},
		{"fneg",         kX86Transparent},
		{"fsqrt",        kX86Transparent},
		{"fabs",         kX86Transparent},
		{"fcmp",         kX86Transparent | kX86WritesFlags},
		{"fcmpflags",    kX86Transparent | kX86WritesFlags},
		{"cvt",          0},
		// sse packed vector
		{"varith",       0},
		{"vunary",       0},
		{"vshiftimm",    0},
		{"vsplat",       0},
		{"vextract",     0},
		{"vpacklane",    0},
		{"vpack",        0},
		{"vpackreg",     0},
		{"vinsertreg",   0},
		{"vshuf",        0},
		// x87
		{"x87loadmem",   0},
		{"x87storemem",  0},
		{"x87loadimmd",  0},
		{"x87fromint",   0},
		{"x87toint",     0},
		{"x87fromsse",   0},
		{"x87tosse",     0},
		{"x87add",       0},
		{"x87sub",       0},
		{"x87mul",       0},
		{"x87div",       0},
		{"x87neg",       0},
		{"x87cmp",       0},
		// control / calls
		{"call",         kX86Transparent | kX86WritesFlags}, // clobbers cover the volatile set, slots are below rsp
		{"ret",          kX86Transparent},
		{"jmp",          kX86Transparent},
		{"switchjump",   kX86Transparent},
		{"br",           kX86Transparent | kX86ReadsFlags},
		// variadic support
		{"vastart",      0},
		// misc
		{"ud2",          0},
		{"prefetch",     0},
		};
		// clang-format on

		static_assert(std::size(kX86OpInfo) == (U32)X86Op::Prefetch + 1, "one per X86Op");

		// clang-format off
		constexpr const C8* kGpNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
		constexpr const C8* kCcNames[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};
		// by memory operand size in bytes
		constexpr const C8* kSizeNames[17] = {"", "byte ", "word ", "", "dword ", "", "", "", "qword ", "", "", "", "", "", "", "", "xmmword "};
		// clang-format on
		constexpr U32 kMnemonicColumn = 16;

		String dispText(I64 disp) {
			if(disp < 0)
				return " - " + std::to_string(-disp);
			if(disp > 0)
				return " + " + std::to_string(disp);
			return "";
		}

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
				return "[rbp" + dispText(operand.slot) + "]";
			case MachineOperand::Kind::Sym:
				return operand.sym();
			case MachineOperand::Kind::Block:
				return "b" + std::to_string(operand.block);
			default:
				return "_";
			}
		}

		// size [base + index*scale + disp]
		String addressText(const MachineInstr& instr, U32 width, U32 indexUse, U32 scaleLog2) {
			const MachineOperand& base = instr.uses[0];
			if(!base.isVReg() && !base.isPhys())
				return operandText(base);
			String text = kSizeNames[width] + ("[" + operandText(base));
			if(indexUse)
				text += " + " + operandText(instr.uses[indexUse]) + "*" + std::to_string(1u << scaleLog2);
			return text + dispText(instr.imm) + "]";
		}

		String memoryOperandText(const MachineInstr& instr) {
			U32 scaleLog2 = (instr.imm2 >> 2) & 3;
			U32 indexed = (instr.imm2 & 2) != 0;
			switch((X86Op)instr.op) {
			case X86Op::Lea:
				return addressText(instr, 0, 1, instr.imm2 & 3);
			case X86Op::Load:
			case X86Op::FLoad:
				return addressText(instr, instr.defs[0].width, indexed, scaleLog2);
			case X86Op::Store:
			case X86Op::FStore: {
				String address = addressText(instr, instr.uses[1].width, 2 * indexed, scaleLog2);
				return address + ", " + operandText(instr.uses[1]);
			}
			case X86Op::Prefetch:
				return addressText(instr, 0, 0, 0);
			default:
				return "";
			}
		}

		B32 immIsCc(const MachineInstr& instr) {
			X86Op op = (X86Op)instr.op;
			if(op == X86Op::Br)
				return instr.imm2 != 0;
			return op == X86Op::SetCC || op == X86Op::CMov || op == X86Op::FCmp || op == X86Op::X87Cmp;
		}

		B32 immIsOperand(X86Op op) {
			return op == X86Op::FrameAddr || op == X86Op::VExtract || op == X86Op::VShuf ||
						 isNormalize(op);
		}

		// two-address ops repeat the def as use 0
		B32 repeatsDef(const MachineInstr& instr) {
			if(instr.defs.empty() || instr.uses.empty())
				return false;
			return operandText(instr.defs[0]) == operandText(instr.uses[0]);
		}

		B32 isNormalize(X86Op op) { return op == X86Op::MaskBits || op == X86Op::SignExtBits; }

		// uses[0] = frame slot, uses[1] = source
		B32 isAnySlotStore(const MachineInstr& in) {
			X86Op op = (X86Op)in.op;
			if((op != X86Op::Store && op != X86Op::FStore) || !in.defs.empty() || in.uses.size() != 2)
				return false;
			return in.uses[0].isFrameSlot() && in.uses[1].isPhys();
		}
	} // namespace detail

	const X86OpInfo& x86OpInfo(X86Op op) { return detail::kX86OpInfo[(U32)op]; }

	String x86InstrText(const MachineInstr& instr) {
		X86Op op = (X86Op)instr.op;
		List<String> parts;
		if(detail::immIsCc(instr))
			parts.push_back(detail::kCcNames[instr.imm & 15]);
		for(const MachineOperand& def : instr.defs)
			parts.push_back(detail::operandText(def));
		String memory = detail::memoryOperandText(instr);
		if(!memory.empty())
			parts.push_back(memory);
		else
			for(U32 i = detail::repeatsDef(instr); i < instr.uses.size(); ++i)
				parts.push_back(detail::operandText(instr.uses[i]));
		// a set swap bit compares use 1 against use 0
		if((op == X86Op::FCmp || op == X86Op::FCmpFlags || op == X86Op::X87Cmp) && instr.imm2)
			std::swap(parts[parts.size() - 1], parts[parts.size() - 2]);
		if(detail::immIsOperand(op))
			parts.push_back(std::to_string(instr.imm));
		String text = x86OpInfo(op).mnemonic;
		if(parts.empty())
			return text;
		text.resize(detail::kMnemonicColumn, ' ');
		for(U32 i = 0; i < parts.size(); ++i) {
			if(i)
				text += ", ";
			text += parts[i];
		}
		return text;
	}
} // namespace rat
