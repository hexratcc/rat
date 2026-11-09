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
		enum class Kind : U8 {
			None,
			VReg,
			Phys,
			Imm,
			FrameSlot,
			Sym,
			Block
		};

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
		static MachineOperand none(U32 w = 8);
		static MachineOperand make(Kind k, U32 w, U32 v);

		B32 isVReg() const { return kind == Kind::VReg; }
		B32 isPhys() const { return kind == Kind::Phys; }
	};

	// a debug var inst lives in loc from here on, a None loc is optimized out
	struct DbgRec {
		U32 inst;
		MachineOperand loc;
		U32 next; // in MachineFunc::dbg, 0 ends the chain
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
		U32 loc = 0;					// DebugBody loc id
		U32 dbg = 0;					// DbgRec chain applied before this instr
	};

	struct MachineBlock {
		I32 id = -1;
		I32 loopDepth = 0; // natural loops containing this block
		U32 align = 0;		 // start offset alignment in bytes, 0 = none
		SmallList<I32, 2> preds;
		SmallList<I32, 2> succs;
		List<MachineInstr> insts;
		U32 dbgEnd = 0; // DbgRec chain applied past the last instr

		U32& dbgBefore(U32 k); // the chain applied before instr k, dbgEnd past the last
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
		List<DbgRec> dbg; // chain storage, 0 unused

		VReg newVReg(U32 cls);
		// a chain is its head index in dbg, 0 empty. addDbg appends in place, joinDbg returns first then second
		void addDbg(U32& chain, U32 inst, MachineOperand loc);
		U32 joinDbg(U32 first, U32 second);
		U32 copyDbg(U32 chain);
		void reset(); // keeps the block storage
	};
} // namespace rat

#endif
