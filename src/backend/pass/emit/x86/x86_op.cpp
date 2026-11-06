#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		// clang-format off
		const X86OpInfo kX86OpInfo[] = {
		//mnemonic       cls
		// pseudo / data movement
		{"copy",         kGp},
		{"loadimm",      kGp},
		{"loadsym",      kGp},
		{"frameaddr",    kGp},
		{"retaddr",      kGp},
		{"lea",          kGp},
		// dynamic stack
		{"stackalloc",   kGp},
		{"stacksave",    kGp},
		{"stackrestore", kGp},
		// non-local goto
		{"setjmp",       kGp},
		{"longjmp",      kGp},
		// integer memory
		{"load",         kGp},
		{"store",        kGp},
		// integer ALU
		{"add",          kGp},
		{"sub",          kGp},
		{"mul",          kGp},
		{"and",          kGp},
		{"or",           kGp},
		{"xor",          kGp},
		{"neg",          kGp},
		{"not",          kGp},
		{"shl",          kGp},
		{"ashr",         kGp},
		{"lshr",         kGp},
		{"rotl",         kGp},
		{"rotr",         kGp},
		{"sdiv",         kGp},
		{"srem",         kGp},
		{"udiv",         kGp},
		{"urem",         kGp},
		{"bsf",          kGp},
		{"bsr",          kGp},
		{"cmp",          kGp},
		{"setcc",        kGp},
		{"cmov",         kGp},
		{"maskbits",     kGp},
		{"signextbits",  kGp},
		{"bswap",        kGp},
		// sse scalar float
		{"fload",        kFp},
		{"fstore",       kFp},
		{"fadd",         kFp},
		{"fsub",         kFp},
		{"fmul",         kFp},
		{"fdiv",         kFp},
		{"fneg",         kFp},
		{"fsqrt",        kFp},
		{"fabs",         kFp},
		{"fcmp",         kGp},
		{"fcmpflags",    kFp},
		{"cvt",          kFp},
		// sse packed vector
		{"varith",       kFp},
		{"vunary",       kFp},
		{"vshiftimm",    kFp},
		{"vsplat",       kFp},
		{"vextract",     kFp},
		{"vpacklane",    kFp},
		{"vpack",        kFp},
		{"vpackreg",     kFp},
		{"vinsertreg",   kFp},
		{"vshuf",        kFp},
		// x87
		{"x87loadmem",   kX87},
		{"x87storemem",  kX87},
		{"x87loadimmd",  kX87},
		{"x87fromint",   kX87},
		{"x87toint",     kGp},
		{"x87fromsse",   kX87},
		{"x87tosse",     kFp},
		{"x87add",       kX87},
		{"x87sub",       kX87},
		{"x87mul",       kX87},
		{"x87div",       kX87},
		{"x87neg",       kX87},
		{"x87cmp",       kGp},
		// control / calls
		{"call",         kGp},
		{"ret",          kGp},
		{"jmp",          kGp},
		{"switchjump",   kGp},
		{"br",           kGp},
		// variadic support
		{"vastart",      kGp},
		{"vaarg",        kGp},
		// misc
		{"ud2",          kGp},
		{"prefetch",     kGp},
		};
		// clang-format on

		static_assert(sizeof(kX86OpInfo) / sizeof(kX86OpInfo[0]) == (U32)X86Op::Prefetch + 1,
									"kX86OpInfo must cover every X86Op");

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
				if(operand.phys >= X86Target::kStBase)
					return "st" + std::to_string(operand.phys - X86Target::kStBase);
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
			case X86Op::FStore:
				return addressText(instr, instr.uses[1].width, 2 * indexed, scaleLog2) + ", " +
							 operandText(instr.uses[1]);
			case X86Op::Prefetch:
				return addressText(instr, 0, 0, 0);
			default:
				return "";
			}
		}

		B32 immIsCc(const MachineInstr& instr) {
			X86Op op = (X86Op)instr.op;
			return op == X86Op::SetCC || op == X86Op::CMov || op == X86Op::FCmp || op == X86Op::X87Cmp ||
						 (op == X86Op::Br && instr.imm2);
		}

		B32 immIsOperand(X86Op op) {
			return op == X86Op::FrameAddr || op == X86Op::MaskBits || op == X86Op::SignExtBits ||
						 op == X86Op::VExtract || op == X86Op::VShuf;
		}

		// two-address ops repeat the def as use 0
		B32 repeatsDef(const MachineInstr& instr) {
			if(instr.defs.empty() || instr.uses.empty())
				return false;
			return operandText(instr.defs[0]) == operandText(instr.uses[0]);
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
