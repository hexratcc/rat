#include "Pass/Emit/X86Layout.h"

#include "CodeGen/MachineFunction.h"
#include "CodeGen/MachineModule.h"
#include "IR/Module.h"
#include "Pass/Emit/X86Op.h"

namespace rat {
	namespace {
		B32 isPureTestBlock(const MachineBlock& b) {
			if(b.insts.empty() || b.insts.size() > 3)
				return false;
			const MachineInstr& last = b.insts.back();
			if((X86Op)last.op != X86Op::Br)
				return false;
			for(U32 i = 0; i + 1 < (U32)b.insts.size(); ++i) {
				const MachineInstr& in = b.insts[(U32)i];
				X86Op op = (X86Op)in.op;
				if(op != X86Op::Cmp && op != X86Op::FCmp)
					return false;
				if(!in.defs.empty() || in.isCall)
					return false;
			}
			return true;
		}

		U32 runOnFunction(MachineFunc& mf) {
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
				for(const MachineInstr& in : b.insts)
					p.insts.push_back(in);

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
	} // namespace

	B32 X86LayoutPass::run(Module& module, MachineModule& mm, const TargetInfo&) {
		U32 changed = 0;
		for(const Function* fn : module)
			changed += runOnFunction(mm.get(fn));
		return changed != 0;
	}
} // namespace rat
