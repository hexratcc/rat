#include "CodeGen/MachineFunction.h"

namespace rat {
	MachineOperand MachineOperand::vr(VReg v, U32 w) {
		MachineOperand o;
		o.kind = Kind::VReg;
		o.vreg = v;
		o.width = w;
		return o;
	}

	MachineOperand MachineOperand::fixed(PhysReg p, U32 w) {
		MachineOperand o;
		o.kind = Kind::Phys;
		o.phys = p;
		o.width = w;
		return o;
	}

	MachineOperand MachineOperand::immVal(I64 v, U32 w) {
		MachineOperand o;
		o.kind = Kind::Imm;
		o.imm = v;
		o.width = w;
		return o;
	}

	MachineOperand MachineOperand::frameSlot(I32 s, U32 w) {
		MachineOperand o;
		o.kind = Kind::FrameSlot;
		o.slot = s;
		o.width = w;
		return o;
	}

	MachineOperand MachineOperand::symbol(const String& s) {
		MachineOperand o;
		o.kind = Kind::Sym;
		o.sym = s;
		return o;
	}

	MachineOperand MachineOperand::blockRef(I32 b) {
		MachineOperand o;
		o.kind = Kind::Block;
		o.block = b;
		return o;
	}

	VReg MachineFunc::newVReg(U32 cls) {
		VReg v = nextVReg++;
		vregClass[v] = cls;
		return v;
	}
} // namespace rat
