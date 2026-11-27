#include "pass/emit/x86/x86_layout.h"

#include "codegen/machine_function.h"
#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		B32 isPureTestBlock(const MachineBlock& b) {
			if(b.insts.empty() || b.insts.size() > 3 || (X86Op)b.insts.back().op != X86Op::Br)
				return false;
			for(U32 i = 0; i + 1 < (U32)b.insts.size(); ++i) {
				const MachineInstr& in = b.insts[i];
				if((X86Op)in.op != X86Op::Cmp || !in.defs.empty() || in.isCall)
					return false;
			}
			return true;
		}

		// the pure test block p jumps to, -1 when none
		I32 jumpedTestBlock(const MachineFunc& mf, const MachineBlock& p) {
			if(p.id < 0 || p.insts.empty() || (X86Op)p.insts.back().op != X86Op::Jmp)
				return -1;
			I32 t = p.insts.back().uses[0].block;
			if(t < 0 || t >= (I32)mf.blocks.size() || t == p.id)
				return -1;
			const MachineBlock& b = mf.blocks[(U32)t];
			if(b.id < 0 || !isPureTestBlock(b))
				return -1;
			return t;
		}

		void copyTestBlock(MachineFunc& mf, MachineBlock& p, const MachineBlock& b) {
			U32 dbg = mf.joinDbg(p.insts.back().dbg, p.dbgEnd);
			p.insts.pop_back();
			U32 first = (U32)p.insts.size();
			p.insts.insert(p.insts.end(), b.insts.begin(), b.insts.end());
			for(U32 i = first; i < (U32)p.insts.size(); ++i)
				p.insts[i].dbg = mf.copyDbg(p.insts[i].dbg);
			p.insts[first].dbg = mf.joinDbg(dbg, p.insts[first].dbg);
			p.dbgEnd = mf.copyDbg(b.dbgEnd);
		}

		void moveTestEdges(MachineFunc& mf, MachineBlock& p, MachineBlock& b) {
			p.succs.erase(std::remove(p.succs.begin(), p.succs.end(), b.id), p.succs.end());
			b.preds.erase(std::remove(b.preds.begin(), b.preds.end(), p.id), b.preds.end());
			for(I32 s : b.succs) {
				p.succs.push_back(s);
				mf.blocks[(U32)s].preds.push_back(p.id);
			}
		}

		U32 duplicateTestBlocks(MachineFunc& mf) {
			U32 changed = 0;
			for(MachineBlock& p : mf.blocks) {
				I32 t = jumpedTestBlock(mf, p);
				if(t < 0)
					continue;
				MachineBlock& b = mf.blocks[(U32)t];
				copyTestBlock(mf, p, b);
				moveTestEdges(mf, p, b);
				++changed;
			}
			return changed;
		}

		B32 endsInBranch(const MachineBlock& b) {
			if(b.insts.empty())
				return false;
			X86Op op = (X86Op)b.insts.back().op;
			return op == X86Op::Jmp || op == X86Op::Br || op == X86Op::SwitchJump;
		}

		// the final target of a chain of blocks that only jump
		I32 resolveJump(const MachineFunc& mf, I32 id) {
			for(U32 hops = 0; hops < 16 && id >= 0 && id < (I32)mf.blocks.size(); ++hops) {
				const MachineBlock& b = mf.blocks[(U32)id];
				if(b.id < 0 || b.insts.size() != 1 || (X86Op)b.insts[0].op != X86Op::Jmp)
					break;
				id = b.insts[0].uses[0].block;
			}
			return id;
		}

		U32 forwardJumpChains(MachineFunc& mf) {
			U32 changed = 0;
			for(MachineBlock& b : mf.blocks) {
				if(b.id < 0 || !endsInBranch(b))
					continue;
				for(MachineOperand& u : b.insts.back().uses)
					if(u.isBlock()) {
						I32 r = resolveJump(mf, u.block);
						if(r != u.block) {
							u.block = r;
							++changed;
						}
					}
			}
			return changed;
		}

		// a branch to a block still on the depth-first search stack is a back edge,
		// its target a loop header. returns the search state by id, nonzero once reached
		List<U8> alignLoopHeaders(MachineFunc& mf, U32 entry) {
			List<U8> state(mf.blocks.size(), 0); // 0 unseen, 1 on the stack, 2 done
			List<U32> stack{entry};
			List<U32> cursor{0}; // next branch operand of each stacked block
			state[entry] = 1;
			while(!stack.empty()) {
				const MachineBlock& b = mf.blocks[stack.back()];
				U32 k = cursor.back()++;
				if(!endsInBranch(b) || k >= (U32)b.insts.back().uses.size()) {
					state[(U32)b.id] = 2;
					stack.pop_back();
					cursor.pop_back();
					continue;
				}
				const MachineOperand& u = b.insts.back().uses[k];
				if(!u.isBlock() || u.block < 0)
					continue;
				if(state[(U32)u.block] == 1)
					mf.blocks[(U32)u.block].align = kLoopAlign;
				if(state[(U32)u.block] != 0)
					continue;
				state[(U32)u.block] = 1;
				stack.push_back((U32)u.block);
				cursor.push_back(0);
			}
			return state;
		}

		// prefer the last block operand (else/jump target),
		// the edge the encoder can elide when adjacent
		I32 chainSuccessor(const MachineBlock& b, const List<B32>& placed, const List<U8>& reached) {
			if(!endsInBranch(b))
				return -1;
			const MachineOperands& uses = b.insts.back().uses;
			for(U32 k = (U32)uses.size(); k-- > 0;) {
				if(!uses[k].isBlock())
					continue;
				I32 t = uses[k].block;
				if(t >= 0 && !placed[(U32)t] && reached[(U32)t])
					return t;
			}
			return -1;
		}

		// order blocks into fallthrough chains, each block followed by a successor
		// where possible so branches fall through, unreachable blocks emptied
		void chainLayout(MachineFunc& mf, const List<U8>& reached) {
			U32 n = (U32)mf.blocks.size();
			List<B32> placed(n, false);
			List<MachineBlock> arranged;
			arranged.reserve(n);
			for(U32 seed = 0; seed < n; ++seed) {
				I32 id = mf.blocks[seed].id;
				while(id >= 0 && reached[(U32)id] && !placed[(U32)id]) {
					placed[(U32)id] = true;
					arranged.push_back(std::move(mf.blocks[(U32)id]));
					id = chainSuccessor(arranged.back(), placed, reached);
				}
			}
			// unreachable blocks stay as empty stubs, ids remain table indices
			for(MachineBlock& stub : mf.blocks) {
				if(stub.id < 0 || placed[(U32)stub.id])
					continue;
				// debug records stay, the successor lists still route through a skipped jump
				for(U32 k = (U32)stub.insts.size(); k-- > 0;) // backward, each join walks one instr chain
					stub.dbgEnd = mf.joinDbg(stub.insts[k].dbg, stub.dbgEnd);
				stub.insts.clear();
				arranged.push_back(std::move(stub));
			}
			mf.blocks = std::move(arranged);
		}

		// a second layout run needs every block back at the index of its id
		void blocksById(MachineFunc& mf) {
			List<MachineBlock> byId(blockIdBound(mf));
			for(MachineBlock& b : byId)
				b.id = -1;
			for(MachineBlock& b : mf.blocks)
				if(b.id >= 0)
					byId[(U32)b.id] = std::move(b);
			mf.blocks = std::move(byId);
		}
	} // namespace detail

	// block ids are block indices after blocksById, chainLayout reorders the blocks.
	// the loop header search also tells which blocks the entry reaches
	B32 X86LayoutPass::run(Module&, const Function&, MachineFunc& mf, const TargetInfo&) {
		detail::blocksById(mf);
		U32 changed = detail::duplicateTestBlocks(mf);
		changed += detail::forwardJumpChains(mf);
		U32 entry = 0;
		while(entry < (U32)mf.blocks.size() && mf.blocks[entry].id < 0)
			++entry;
		if(entry < (U32)mf.blocks.size())
			detail::chainLayout(mf, detail::alignLoopHeaders(mf, entry));
		return changed != 0;
	}
} // namespace rat
