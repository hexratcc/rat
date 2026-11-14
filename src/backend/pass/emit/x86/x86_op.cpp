#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		// clang-format off
		const X86OpInfo kX86OpInfo[] = {
		//mnemonic       cls   flags
		// pseudo / data movement
		{"copy",         kGp,  kX86Transparent | kX86KeepsLowBits},
		{"loadimm",      kGp,  kX86Transparent},
		{"loadsym",      kGp,  kX86Transparent},
		{"frameaddr",    kGp,  kX86Transparent},
		{"retaddr",      kGp,  kX86Transparent},
		{"lea",          kGp,  kX86Transparent | kX86KeepsLowBits},
		// dynamic stack
		{"stackalloc",   kGp,  0},
		{"stacksave",    kGp,  0},
		{"stackrestore", kGp,  0},
		// non-local goto
		{"setjmp",       kGp,  0},
		{"longjmp",      kGp,  0},
		// integer memory
		{"load",         kGp,  kX86Transparent | kX86KeepsLowBits},
		{"store",        kGp,  kX86Transparent},
		// integer ALU
		{"add",          kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"sub",          kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"mul",          kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"and",          kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"or",           kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"xor",          kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"neg",          kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"not",          kGp,  kX86Transparent | kX86KeepsLowBits},
		{"shl",          kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"ashr",         kGp,  kX86Transparent | kX86WritesFlags},
		{"lshr",         kGp,  kX86Transparent | kX86WritesFlags},
		{"rotl",         kGp,  kX86Transparent | kX86WritesFlags},
		{"rotr",         kGp,  kX86Transparent | kX86WritesFlags},
		{"sdiv",         kGp,  kX86WritesFlags},
		{"srem",         kGp,  kX86WritesFlags},
		{"udiv",         kGp,  kX86WritesFlags},
		{"urem",         kGp,  kX86WritesFlags},
		{"bsf",          kGp,  kX86Transparent | kX86WritesFlags},
		{"bsr",          kGp,  kX86Transparent | kX86WritesFlags},
		{"cmp",          kGp,  kX86Transparent | kX86WritesFlags},
		{"setcc",        kGp,  kX86Transparent | kX86ReadsFlags},
		{"cmov",         kGp,  kX86Transparent | kX86ReadsFlags | kX86KeepsLowBits},
		{"maskbits",     kGp,  kX86Transparent | kX86WritesFlags | kX86KeepsLowBits},
		{"signextbits",  kGp,  kX86Transparent | kX86KeepsLowBits},
		{"bswap",        kGp,  kX86Transparent},
		// sse scalar float
		{"fload",        kFp,  kX86Transparent},
		{"fstore",       kFp,  kX86Transparent},
		{"fadd",         kFp,  kX86Transparent},
		{"fsub",         kFp,  kX86Transparent},
		{"fmul",         kFp,  kX86Transparent},
		{"fdiv",         kFp,  kX86Transparent},
		{"fneg",         kFp,  kX86Transparent},
		{"fsqrt",        kFp,  kX86Transparent},
		{"fabs",         kFp,  kX86Transparent},
		{"fcmp",         kGp,  kX86Transparent | kX86WritesFlags},
		{"fcmpflags",    kFp,  kX86Transparent | kX86WritesFlags},
		{"cvt",          kFp,  0},
		// sse packed vector
		{"varith",       kFp,  0},
		{"vunary",       kFp,  0},
		{"vshiftimm",    kFp,  0},
		{"vsplat",       kFp,  0},
		{"vextract",     kFp,  0},
		{"vpacklane",    kFp,  0},
		{"vpack",        kFp,  0},
		{"vpackreg",     kFp,  0},
		{"vinsertreg",   kFp,  0},
		{"vshuf",        kFp,  0},
		// x87
		{"x87loadmem",   kX87, 0},
		{"x87storemem",  kX87, 0},
		{"x87loadimmd",  kX87, 0},
		{"x87fromint",   kX87, 0},
		{"x87toint",     kGp,  0},
		{"x87fromsse",   kX87, 0},
		{"x87tosse",     kFp,  0},
		{"x87add",       kX87, 0},
		{"x87sub",       kX87, 0},
		{"x87mul",       kX87, 0},
		{"x87div",       kX87, 0},
		{"x87neg",       kX87, 0},
		{"x87cmp",       kGp,  0},
		// control / calls
		{"call",         kGp,  kX86Transparent | kX86WritesFlags}, // clobbers cover the volatile set, slots are below rsp
		{"ret",          kGp,  kX86Transparent},
		{"jmp",          kGp,  kX86Transparent},
		{"switchjump",   kGp,  kX86Transparent},
		{"br",           kGp,  kX86Transparent | kX86ReadsFlags},
		// variadic support
		{"vastart",      kGp,  0},
		{"vaarg",        kGp,  0},
		// misc
		{"ud2",          kGp,  0},
		{"prefetch",     kGp,  0},
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

		// bytes in the low 32 bits, then the alignment up to 16, the eightbyte count and 2 bits per class
		I64 packVaAggregate(const VaAggregate& agg) {
			U64 desc = agg.bytes | (U64)std::min(agg.align, 16u) << 32 | (U64)agg.regs.count << 40;
			for(U32 k = 0; k < 2; ++k)
				desc |= (U64)agg.regs.cls[k] << (42 + 2 * k);
			return (I64)desc;
		}

		VaAggregate unpackVaAggregate(I64 desc) {
			U64 bits = (U64)desc;
			VaAggregate agg;
			agg.bytes = (U32)bits;
			agg.align = (U32)(bits >> 32 & 0xff);
			agg.regs.count = (U8)(bits >> 40 & 3);
			for(U32 k = 0; k < 2; ++k)
				agg.regs.cls[k] = (AbiClass)(bits >> (42 + 2 * k) & 3);
			return agg;
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
			return in.uses[0].kind == MachineOperand::Kind::FrameSlot && in.uses[1].isPhys();
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
