#include "CodeGen/LIR.h"

namespace rat {
	MOperand MOperand::vr(VReg v, U32 w) {
		MOperand o;
		o.kind = Kind::VReg;
		o.vreg = v;
		o.width = w;
		return o;
	}

	MOperand MOperand::fixed(PhysReg p, U32 w) {
		MOperand o;
		o.kind = Kind::Phys;
		o.phys = p;
		o.width = w;
		return o;
	}

	MOperand MOperand::immVal(I64 v, U32 w) {
		MOperand o;
		o.kind = Kind::Imm;
		o.imm = v;
		o.width = w;
		return o;
	}

	MOperand MOperand::frameSlot(I32 s, U32 w) {
		MOperand o;
		o.kind = Kind::FrameSlot;
		o.slot = s;
		o.width = w;
		return o;
	}

	MOperand MOperand::symbol(const String& s) {
		MOperand o;
		o.kind = Kind::Sym;
		o.sym = s;
		return o;
	}

	MOperand MOperand::blockRef(I32 b) {
		MOperand o;
		o.kind = Kind::Block;
		o.block = b;
		return o;
	}

	VReg LirFunc::newVReg(U32 cls) {
		VReg v = nextVReg++;
		vregClass[v] = cls;
		return v;
	}
} // namespace rat
