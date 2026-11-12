#include "pass/emit/x86/x86_peephole.h"

#include "codegen/machine_function.h"

namespace rat {
	// what the dropped instr would have written now holds a wrong value, past the bits a normalize
	// leaves right
	void X86PeepholePass::killWrite(MachineFunc& mf, U32& chain, const MachineInstr& dropped) {
		MachineOperand loc = dropped.defs.empty() ? dropped.uses[0] : dropped.defs[0];
		loc.width = detail::isNormalize((X86Op)dropped.op) ? (U8)dropped.imm : 0;
		mf.addDbg(chain, kDbgKill, loc);
	}

	// a killed reg may differ in poison from the other locations holding its value
	void X86PeepholePass::noteKills(const MachineFunc& mf, U32 chain, List<U32>& diverged) const {
		for(U32 r = chain; r; r = mf.dbg[r].next)
			if(mf.dbg[r].inst == kDbgKill && mf.dbg[r].loc.isPhys())
				diverged.push_back(st.valueOf(mf.dbg[r].loc.phys));
	}

	// a fold reads the value it finds in place through its uses
	B32 X86PeepholePass::holdsDiverged(const MachineInstr& in, const List<U32>& diverged) const {
		for(const MachineOperand& o : in.uses) {
			U32 v = o.isPhys() ? st.valueOf(o.phys) : 0;
			if(o.kind == MachineOperand::Kind::FrameSlot)
				v = st.slotValue(o.slot, o.width);
			if(v && std::find(diverged.begin(), diverged.end(), v) != diverged.end())
				return true;
		}
		return false;
	}
} // namespace rat
