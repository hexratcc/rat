#ifndef RAT_CODEGEN_LIR_H
#define RAT_CODEGEN_LIR_H

#include "Core.h"

#include "Target/Target.h"

namespace rat {
	struct Function;

	using VReg = U32;
	using LirOpcode = U32;

	constexpr VReg kNoVReg = 0;

	struct MOperand {
		enum class Kind { None, VReg, Phys, Imm, FrameSlot, Sym, Block };

		Kind kind = Kind::None;
		VReg vreg = kNoVReg;
		PhysReg phys = kNoReg;
		I64 imm = 0;
		I32 slot = 0;
		String sym;
		I32 block = -1;
		U32 width = 8;

		static MOperand vr(VReg v, U32 w = 8);
		static MOperand fixed(PhysReg p, U32 w = 8);
		static MOperand immVal(I64 v, U32 w = 8);
		static MOperand frameSlot(I32 s, U32 w = 8);
		static MOperand symbol(const String& s);
		static MOperand blockRef(I32 b);

		B32 isVReg() const { return kind == Kind::VReg; }
		B32 isPhys() const { return kind == Kind::Phys; }
	};

	struct MInst {
		LirOpcode op = 0;
		List<MOperand> defs;		// written results
		List<MOperand> uses;		// read operands
		List<PhysReg> clobbers; // extra phys regs destroyed
		U32 regClass = 0;				// register class of the def
		I64 imm = 0;						// backend-defined small immediate
		I64 imm2 = 0;						// second backend-defined immediate
		B32 isCopy = false;			// reg-reg move
		B32 isCall = false;			// applies clobbers and bounds live intervals
	};

	struct MBlock {
		I32 id = -1;
		List<I32> preds;
		List<I32> succs;
		List<MInst> insts;
	};

	struct LirFunc {
		const Function* src = nullptr;
		List<MBlock> blocks;
		U32 nextVReg = 1;
		Map<VReg, U32> vregClass;
		U32 frameBytes = 0;

		VReg newVReg(U32 cls);
	};
} // namespace rat

#endif
