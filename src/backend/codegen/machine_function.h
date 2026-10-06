#ifndef RAT_CODEGEN_MACHINEFUNCTION_H
#define RAT_CODEGEN_MACHINEFUNCTION_H

#include "core.h"

#include "target/target.h"

namespace rat {
	struct Function;

	using VReg = U32;
	using MachineOpcode = U32;

	constexpr VReg kNoVReg = 0;

	// trivial, so inline operand storage costs nothing to construct; build one through the factories
	struct MachineOperand {
		enum class Kind : U8 { None, VReg, Phys, Imm, FrameSlot, Sym, Block };

		Kind kind;
		U8 width;
		union { // by kind
			VReg vreg;
			PhysReg phys;
			I32 slot;
			I32 block;
		};
		union {
			I64 imm;
			const String* name;
		};

		const String& sym() const { return *name; }

		static MachineOperand vr(VReg v, U32 w = 8);
		static MachineOperand fixed(PhysReg p, U32 w = 8);
		static MachineOperand immVal(I64 v, U32 w = 8);
		static MachineOperand frameSlot(I32 s, U32 w = 8);
		static MachineOperand symbol(const String& s);
		static MachineOperand blockRef(I32 b);
		static MachineOperand make(Kind k, U32 w, U32 v);

		B32 isVReg() const { return kind == Kind::VReg; }
		B32 isPhys() const { return kind == Kind::Phys; }
	};

	using MachineDefs = SmallList<MachineOperand, 2>;
	using MachineOperands = SmallList<MachineOperand, 3>;

	struct MachineInstr {
		MachineOpcode op = 0;
		U8 regClass = 0;			// register class of the def
		U8 isCall = false;		// applies clobbers and bounds live intervals
		MachineDefs defs;			// written results
		MachineOperands uses; // read operands
		U64 clobbers = 0;			// extra phys regs destroyed, by bit
		I64 imm = 0;					// backend-defined small immediate
		I64 imm2 = 0;					// second backend-defined immediate
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
