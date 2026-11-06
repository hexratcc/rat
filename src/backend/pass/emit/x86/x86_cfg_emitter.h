// Graphviz DOT dump of the machine block CFG

#ifndef RAT_PASS_EMIT_X86CFGEMITTER_H
#define RAT_PASS_EMIT_X86CFGEMITTER_H

#include "core.h"
#include "pass/pass.h"

#include <iosfwd>

namespace rat {
	struct MachineBlock;

	struct X86CfgEmitterPass : MachinePass {
		explicit X86CfgEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
		void finish(Module& module, const TargetInfo& target) override;
	private:
		void writeBlock(const Function& fn, const MachineBlock& block);
	private:
		std::ostream* os;
		U32 fnIndex = 0;
	};
} // namespace rat

#endif
