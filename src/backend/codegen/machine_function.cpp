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

	MachineOperand MachineOperand::symbol(const String& s) {
		MachineOperand o = make(Kind::Sym, 8, 0);
		o.name = &s;
		return o;
	}

	VReg MachineFunc::newVReg(U32 cls) {
		VReg v = nextVReg++;
		if(vregClass.size() <= v)
			vregClass.resize(v + 1, 0);
		vregClass[v] = cls;
		return v;
	}
} // namespace rat
