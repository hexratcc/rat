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
	struct MachineFunc;
	struct MachineInstr;
	struct MachineOperand;

	namespace detail {
		B32 isPureTestBlock(const MachineBlock& b);
		U32 duplicateTestBlocks(MachineFunc& mf);
		B32 endsInBranch(const MachineBlock& b);
		I32 resolveJump(const MachineFunc& mf, I32 id);
		U32 forwardJumpChains(MachineFunc& mf);
		List<B32> reachableFrom(const List<U32>& first, const List<I32>& succ, I32 entry);
		void chainLayout(MachineFunc& mf);
	} // namespace detail

	struct X86LayoutPass : MachinePass {
		const C8* name() const override { return "x86-layout"; }
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
	};
} // namespace rat

#endif
