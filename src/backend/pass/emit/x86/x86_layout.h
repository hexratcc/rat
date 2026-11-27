// post-RA branch layout: tail-duplicate tiny flag-test blocks into
// predecessors that reach them with an unconditional jump:
//
//   header: cmp; jcc exit          header: cmp; jcc exit
//   body:   ...              ->    body:   ...
//   latch:  jmp header             latch:  cmp; jcc exit / fall into body
//
// then aligns each loop header, the target of a back edge, to 64 bytes so
// that a loop of up to 64 bytes sits in one 64-byte fetch block. Branches
// skip blocks that only jump, and the blocks are ordered into fallthrough chains

#ifndef RAT_PASS_EMIT_X86LAYOUT_H
#define RAT_PASS_EMIT_X86LAYOUT_H

#include "core.h"

#include "pass/pass.h"

namespace rat {
	struct MachineBlock;
	struct MachineFunc;

	namespace detail {
		constexpr U32 kLoopAlign = 64;

		void blocksById(MachineFunc& mf);
		B32 isPureTestBlock(const MachineBlock& b);
		I32 jumpedTestBlock(const MachineFunc& mf, const MachineBlock& p);
		void copyTestBlock(MachineFunc& mf, MachineBlock& p, const MachineBlock& b);
		void moveTestEdges(MachineFunc& mf, MachineBlock& p, MachineBlock& b);
		U32 duplicateTestBlocks(MachineFunc& mf);
		B32 endsInBranch(const MachineBlock& b);
		I32 resolveJump(const MachineFunc& mf, I32 id);
		U32 forwardJumpChains(MachineFunc& mf);
		List<U8> alignLoopHeaders(MachineFunc& mf, U32 entry);
		I32 chainSuccessor(const MachineBlock& b, const List<B32>& placed, const List<U8>& reached);
		void chainLayout(MachineFunc& mf, const List<U8>& reached);
	} // namespace detail

	struct X86LayoutPass : MachinePass {
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
	};
} // namespace rat

#endif
