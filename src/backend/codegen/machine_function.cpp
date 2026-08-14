#include "codegen/machine_function.h"

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

	namespace detail {
		MachineSymTable& machineSyms() {
			static thread_local MachineSymTable t;
			return t;
		}
	} // namespace detail

	U32 internMachineSym(const String& s) {
		if(s.empty())
			return 0;
		detail::MachineSymTable& t = detail::machineSyms();
		auto it = t.ids.find(s);
		if(it != t.ids.end())
			return it->second;
		U32 id = (U32)t.names.size();
		t.names.push_back(s);
		t.ids.emplace(s, id);
		return id;
	}

	const String& machineSymName(U32 id) {
		detail::MachineSymTable& t = detail::machineSyms();
		return id < t.names.size() ? t.names[id] : t.names[0];
	}

	MachineOperand MachineOperand::symbol(const String& s) {
		MachineOperand o;
		o.kind = Kind::Sym;
		o.symId = internMachineSym(s);
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
		if(vregClass.size() <= v)
			vregClass.resize(v + 1, 0);
		vregClass[v] = cls;
		return v;
	}
} // namespace rat
