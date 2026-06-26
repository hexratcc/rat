#ifndef RAT_CODEGEN_MACHINEFUNCTION_H
#define RAT_CODEGEN_MACHINEFUNCTION_H

#include "Core.h"

#include "Target/Target.h"

namespace rat {
	struct Function;

	using VReg = U32;
	using MachineOpcode = U32;

	constexpr VReg kNoVReg = 0;

	struct MachineOperand {
		enum class Kind { None, VReg, Phys, Imm, FrameSlot, Sym, Block };

		Kind kind = Kind::None;
		VReg vreg = kNoVReg;
		PhysReg phys = kNoReg;
		I64 imm = 0;
		I32 slot = 0;
		String sym;
		I32 block = -1;
		U32 width = 8;

		static MachineOperand vr(VReg v, U32 w = 8);
		static MachineOperand fixed(PhysReg p, U32 w = 8);
		static MachineOperand immVal(I64 v, U32 w = 8);
		static MachineOperand frameSlot(I32 s, U32 w = 8);
		static MachineOperand symbol(const String& s);
		static MachineOperand blockRef(I32 b);

		B32 isVReg() const { return kind == Kind::VReg; }
		B32 isPhys() const { return kind == Kind::Phys; }
	};

	struct MachineInstr {
		MachineOpcode op = 0;
		List<MachineOperand> defs; // written results
		List<MachineOperand> uses; // read operands
		List<PhysReg> clobbers;		 // extra phys regs destroyed
		U32 regClass = 0;					 // register class of the def
		I64 imm = 0;							 // backend-defined small immediate
		I64 imm2 = 0;							 // second backend-defined immediate
		B32 isCopy = false;				 // reg-reg move
		B32 isCall = false;				 // applies clobbers and bounds live intervals
	};

	struct MachineBlock {
		I32 id = -1;
		List<I32> preds;
		List<I32> succs;
		List<MachineInstr> insts;
	};

	struct MachineFuncAux {
		virtual ~MachineFuncAux() = default;
	};

	struct MachineFunc {
		const Function* src = nullptr;
		List<MachineBlock> blocks;
		U32 nextVReg = 1;
		Map<VReg, U32> vregClass;
		U32 frameBytes = 0;
		List<PhysReg> usedCalleeSaved;
		UniquePtr<MachineFuncAux> aux;

		VReg newVReg(U32 cls);
	};
} // namespace rat

#endif
