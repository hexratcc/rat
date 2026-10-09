#include "pass/emit/x86/x86_layout.h"

#include "codegen/machine_function.h"
#include "pass/emit/x86/x86_op.h"

namespace rat {
	namespace detail {
		B32 isPureTestBlock(const MachineBlock& b) {
			if(b.insts.empty() || b.insts.size() > 3)
				return false;
			const MachineInstr& last = b.insts.back();
			if((X86Op)last.op != X86Op::Br)
				return false;
			for(U32 i = 0; i + 1 < (U32)b.insts.size(); ++i) {
				const MachineInstr& in = b.insts[i];
				if((X86Op)in.op != X86Op::Cmp || !in.defs.empty() || in.isCall)
					return false;
			}
			return true;
		}

		U32 duplicateTestBlocks(MachineFunc& mf) {
			U32 changed = 0;
			for(MachineBlock& p : mf.blocks) {
				if(p.id < 0 || p.insts.empty())
					continue;
				MachineInstr& term = p.insts.back();
				if((X86Op)term.op != X86Op::Jmp)
					continue;
				I32 t = term.uses[0].block;
				if(t < 0 || t >= (I32)mf.blocks.size() || t == p.id)
					continue;
				MachineBlock& b = mf.blocks[(U32)t];
				if(b.id < 0 || !isPureTestBlock(b))
					continue;

				// replace the jmp with a copy of the test block's instructions
				p.insts.pop_back();
				p.insts.insert(p.insts.end(), b.insts.begin(), b.insts.end());

				// p's successor edge moves from b to b's successors
				p.succs.erase(std::remove(p.succs.begin(), p.succs.end(), t), p.succs.end());
				b.preds.erase(std::remove(b.preds.begin(), b.preds.end(), p.id), b.preds.end());
				for(I32 s : b.succs) {
					p.succs.push_back(s);
					mf.blocks[(U32)s].preds.push_back(p.id);
				}
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

		I32 resolveJump(const MachineFunc& mf, I32 id) {
			U32 hops = 0;
			while(hops++ < 16) {
				if(id < 0 || id >= (I32)mf.blocks.size())
					break;
				const MachineBlock& b = mf.blocks[(U32)id];
				if(b.id < 0 || b.insts.size() != 1)
					break;
				const MachineInstr& only = b.insts[0];
				if((X86Op)only.op != X86Op::Jmp)
					break;
				I32 next = only.uses[0].block;
				if(next == id)
					break; // self-loop
				id = next;
			}
			return id;
		}

		// retarget branches at blocks that only jump, straight to the final target
		U32 forwardJumpChains(MachineFunc& mf) {
			U32 changed = 0;
			for(MachineBlock& b : mf.blocks) {
				if(b.id < 0 || !endsInBranch(b))
					continue;
				for(MachineOperand& u : b.insts.back().uses)
					if(u.kind == MachineOperand::Kind::Block) {
						I32 r = resolveJump(mf, u.block);
						if(r != u.block) {
							u.block = r;
							++changed;
						}
					}
			}
			return changed;
		}

		// reachable from entry
		List<B32> reachableFrom(const List<U32>& first, const List<I32>& succ, I32 entry) {
			U32 n = (U32)first.size() - 1;
			List<B32> reach(n, false);
			List<I32> work{entry};
			reach[(U32)entry] = true;
			while(!work.empty()) {
				I32 id = work.back();
				work.pop_back();
				for(U32 k = first[(U32)id]; k < first[(U32)id + 1]; ++k)
					if(I32 t = succ[k]; t >= 0 && t < (I32)n && !reach[(U32)t]) {
						reach[(U32)t] = true;
						work.push_back(t);
					}
			}
			return reach;
		}

		// order blocks into fallthrough chains: each block followed by a successor
		// where possible so branches fall through; unreachable blocks dropped
		void chainLayout(MachineFunc& mf) {
			U32 n = (U32)mf.blocks.size();
			if(n < 2)
				return;
			List<U32> blockAt(n, 0);
			List<U32> first(n + 1, 0);
			for(U32 i = 0; i < n; ++i)
				if(mf.blocks[i].id >= 0)
					blockAt[(U32)mf.blocks[i].id] = i;
			List<I32> succ;
			for(U32 id = 0; id < n; ++id) {
				const MachineBlock& b = mf.blocks[blockAt[id]];
				if(b.id == (I32)id && endsInBranch(b))
					for(const MachineOperand& u : b.insts.back().uses)
						if(u.kind == MachineOperand::Kind::Block)
							succ.push_back(u.block);
				first[id + 1] = (U32)succ.size();
			}

			I32 entry = -1;
			for(U32 i = 0; i < n && entry < 0; ++i)
				if(mf.blocks[i].id >= 0)
					entry = mf.blocks[i].id;
			if(entry < 0)
				return;

			List<B32> reach = reachableFrom(first, succ, entry);

			// greedy: prefer the last block operand (else/jump target), the edge
			// the encoder can elide when adjacent
			List<B32> placed(n, false);
			List<U32> order;
			order.reserve(n);
			for(U32 seedIdx = 0; seedIdx < n; ++seedIdx) {
				I32 seed = mf.blocks[seedIdx].id;
				if(seed < 0 || placed[(U32)seed] || !reach[(U32)seed])
					continue;
				I32 id = seed;
				while(id >= 0 && !placed[(U32)id]) {
					placed[(U32)id] = true;
					order.push_back(blockAt[(U32)id]);
					I32 next = -1;
					for(U32 k = first[(U32)id + 1]; k-- > first[(U32)id];)
						if(I32 t = succ[k]; t >= 0 && !placed[(U32)t] && reach[(U32)t]) {
							next = t;
							break;
						}
					id = next;
				}
			}

			List<MachineBlock> arranged;
			arranged.reserve(n);
			for(U32 idx : order)
				arranged.push_back(std::move(mf.blocks[idx]));
			// keep unreachable blocks as empty stubs so ids stay valid table indices
			for(U32 i = 0; i < n; ++i) {
				I32 id = mf.blocks[i].id;
				if(id >= 0 && !placed[(U32)id]) {
					MachineBlock stub = std::move(mf.blocks[i]);
					stub.insts.clear();
					arranged.push_back(std::move(stub));
				}
			}
			mf.blocks = std::move(arranged);
		}
	} // namespace detail

	B32 X86LayoutPass::run(Module&, const Function&, MachineFunc& mf, const TargetInfo&) {
		U32 changed = detail::duplicateTestBlocks(mf);
		changed += detail::forwardJumpChains(mf);
		detail::chainLayout(mf);
		return changed != 0;
	}
} // namespace rat
