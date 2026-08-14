// post-RA branch layout: tail-duplicate tiny flag-test blocks into
// predecessors that reach them with an unconditional jump:
//
//   header: cmp; jcc exit          header: cmp; jcc exit
//   body:   ...              ->    body:   ...
//   latch:  jmp header             latch:  cmp; jcc exit / fall into body

#ifndef RAT_PASS_EMIT_X86LAYOUT_H
#define RAT_PASS_EMIT_X86LAYOUT_H

#include "core.h"

#include "pass/pass.h"

namespace rat {
	struct MachineBlock;
	struct MachineOperand;

	namespace detail {
		B32 isPureTestBlock(const MachineBlock& b);
		U32 runOnFunction(MachineFunc& mf);
		B32 isBlockRef(const MachineOperand& o);
		U32 forwardJumpChains(MachineFunc& mf);
		void chainLayout(MachineFunc& mf);
	} // namespace detail

	struct X86LayoutPass : MachinePass {
		const C8* name() const override { return "x86-layout"; }
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	};
} // namespace rat

#endif
