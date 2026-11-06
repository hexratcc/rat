#include "pass/emit/x86/x86_cfg_emitter.h"

#include "codegen/machine_function.h"
#include "ir/function.h"
#include "pass/emit/x86/x86_op.h"

#include <ostream>

namespace rat {
	X86CfgEmitterPass::X86CfgEmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* X86CfgEmitterPass::name() const { return "x86-cfg-emitter"; }

	void X86CfgEmitterPass::writeBlock(const Function& fn, const MachineBlock& block) {
		String nodePrefix = "f" + std::to_string(fnIndex) + "_b";
		*os << "  " << nodePrefix << block.id << " [label=\"" << fn.getName() << " b" << block.id;
		if(block.loopDepth)
			*os << " (loop " << block.loopDepth << ")";
		*os << "\\l\\l";
		for(const MachineInstr& instr : block.insts)
			*os << x86InstrText(instr) << "\\l";
		*os << "\"];\n";
		for(const MachineOperand& use : block.insts.back().uses)
			if(use.kind == MachineOperand::Kind::Block)
				*os << "  " << nodePrefix << block.id << " -> " << nodePrefix << use.block << ";\n";
	}

	B32 X86CfgEmitterPass::run(Module&, const Function& fn, MachineFunc& mf, const TargetInfo&) {
		if(fnIndex == 0)
			*os << "strict digraph rat {\n  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
		for(const MachineBlock& block : mf.blocks)
			if(block.id >= 0 && !block.insts.empty())
				writeBlock(fn, block);
		++fnIndex;
		return false;
	}

	void X86CfgEmitterPass::finish(Module&, const TargetInfo&) {
		if(fnIndex == 0)
			*os << "strict digraph rat {\n";
		*os << "}\n";
	}
} // namespace rat
