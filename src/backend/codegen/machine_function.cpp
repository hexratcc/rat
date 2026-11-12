#include "codegen/machine_function.h"

namespace rat {
	MachineOperand MachineOperand::make(Kind k, U32 w, U32 v) {
		MachineOperand o;
		o.kind = k;
		o.width = (U8)w;
		o.vreg = v;
		o.imm = 0;
		return o;
	}

	MachineOperand MachineOperand::vr(VReg v, U32 w) { return make(Kind::VReg, w, v); }
	MachineOperand MachineOperand::fixed(PhysReg p, U32 w) { return make(Kind::Phys, w, p); }
	MachineOperand MachineOperand::frameSlot(I32 s, U32 w) {
		return make(Kind::FrameSlot, w, (U32)s);
	}
	MachineOperand MachineOperand::blockRef(I32 b) { return make(Kind::Block, 8, (U32)b); }

	MachineOperand MachineOperand::immVal(I64 v, U32 w) {
		MachineOperand o = make(Kind::Imm, w, 0);
		o.imm = v;
		return o;
	}

	MachineOperand MachineOperand::none(U32 w) { return make(Kind::None, w, 0); }

	MachineOperand MachineOperand::symbol(const String& s) {
		MachineOperand o = make(Kind::Sym, 8, 0);
		o.name = &s;
		return o;
	}

	U32& MachineBlock::dbgBefore(U32 k) { return k < insts.size() ? insts[k].dbg : dbgEnd; }

	void MachineFunc::reset() {
		for(MachineBlock& b : blocks) {
			b.id = -1;
			b.loopDepth = 0;
			b.align = 0;
			b.preds.clear();
			b.succs.clear();
			b.insts.clear();
			b.dbgEnd = 0;
		}
		dbg.assign(1, {});
		nextVReg = 1;
		vregClass.clear();
		frameBytes = 0;
		usedCalleeSaved.clear();
		aux.reset();
	}

	VReg MachineFunc::newVReg(U32 cls) {
		VReg v = nextVReg++;
		if(vregClass.size() <= v)
			vregClass.resize(v + 1, 0);
		vregClass[v] = cls;
		return v;
	}

	void MachineFunc::addDbg(U32& chain, U32 inst, MachineOperand loc) {
		dbg.push_back({inst, loc, 0});
		chain = joinDbg(chain, (U32)dbg.size() - 1);
	}

	U32 MachineFunc::joinDbg(U32 first, U32 second) {
		if(!first || !second)
			return first ? first : second;
		U32 tail = first;
		while(dbg[tail].next)
			tail = dbg[tail].next;
		dbg[tail].next = second;
		return first;
	}

	U32 MachineFunc::copyDbg(U32 chain) {
		U32 head = 0;
		for(U32 r = chain; r; r = dbg[r].next)
			addDbg(head, dbg[r].inst, dbg[r].loc);
		return head;
	}
} // namespace rat
