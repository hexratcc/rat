#ifndef RAT_CODEGEN_MACHINEFUNCTION_H
#define RAT_CODEGEN_MACHINEFUNCTION_H

#include "core.h"

#include "target/target.h"

namespace rat {
	struct Function;

	using VReg = U32;
	using MachineOpcode = U32;

	constexpr VReg kNoVReg = 0;

	namespace detail {
		struct MachineSymTable {
			List<String> names{String()}; // id 0 is the empty name
			Map<String, U32> ids;
		};
		MachineSymTable& machineSyms();
	} // namespace detail

	U32 internMachineSym(const String& s);
	const String& machineSymName(U32 id);

	struct MachineOperand {
		enum class Kind { None, VReg, Phys, Imm, FrameSlot, Sym, Block };

		Kind kind = Kind::None;
		VReg vreg = kNoVReg;
		PhysReg phys = kNoReg;
		I64 imm = 0;
		I32 slot = 0;
		U32 symId = 0;
		I32 block = -1;
		U32 width = 8;

		const String& sym() const { return machineSymName(symId); }

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
		B32 isCall = false;				 // applies clobbers and bounds live intervals
	};

	struct MachineBlock {
		I32 id = -1;
		I32 loopDepth = 0; // natural loops containing this block
		List<I32> preds;
		List<I32> succs;
		List<MachineInstr> insts;
	};

	struct MachineFuncAux {
		virtual ~MachineFuncAux() = default;
	};

	struct MachineFunc {
		List<MachineBlock> blocks;
		U32 nextVReg = 1;
		List<U32> vregClass;
		U32 frameBytes = 0;
		List<PhysReg> usedCalleeSaved;
		UniquePtr<MachineFuncAux> aux;

		VReg newVReg(U32 cls);
	};
} // namespace rat

#endif
