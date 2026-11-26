#ifndef RAT_CODEGEN_REGALLOC_H
#define RAT_CODEGEN_REGALLOC_H

#include "core.h"

#include "codegen/machine_function.h"
#include "pass/pass.h"

#include <tuple>

namespace rat {
	namespace detail {
		PhysReg firstFree(const List<PhysReg>& regs, U64 blocked);
		U32 loopWeight(I32 depth);
	} // namespace detail

	struct RegAllocPass : MachinePass {
		B32 run(Module& module, const Function& fn, MachineFunc& mf, const TargetInfo& target) override;
		// -O0 mode, a vreg live across blocks stays on the stack
		void setLocal(B32 v) { local = v; }
	private:
		using Seg = Pair<I32, I32>;
		using Copy = std::tuple<I32, VReg, VReg>; // (-loop weight, def, source)

		struct Interval {
			List<Seg> segs;
			F32 weight = 0;
			VReg root = kNoVReg;
			PhysReg hint = kNoReg;
			PhysReg reg = kNoReg;
			I32 slot = 0;

			void reset(VReg self); // keeps the segment storage
		};

		void setup(const TargetInfo& target);
		void number();
		// intervals
		void pinFixed(const MachineInstr& in, U64 u, B32 copy, U64& live);
		void liveness();
		void buildIntervals(List<Copy>& copies);
		void addSeg(VReg v, I32 start, I32 end);
		void noteCopy(List<Copy>& copies, const MachineInstr& in, U32 weight);
		// assignment
		void coalesce(List<Copy>& copies);
		VReg find(VReg v);
		B32 overlaps(VReg a, VReg b) const;
		void merge(VReg a, VReg b, List<Seg>& merged);
		void assignRegs();
		void assignSlots(List<Pair<I32, VReg>>& spilled);
		PhysReg pick(VReg v) const;
		// debug records
		B32 covers(VReg v, I32 pos) const;
		void checkDebugLive();
		void rewriteDebug();
		// rewrite
		void rewrite();
		B32 isCopy(const MachineInstr& in) const;
		B32 rewriteCopy(List<MachineInstr>& out, const MachineInstr& in, U32 i);
		void rewriteInstr(List<MachineInstr>& out, MachineInstr& in, U32 i);
		PhysReg
		spillReg(List<MachineInstr>& out, const MachineOperand& o, U32 i, U64 clobbers, B32 use);
		PhysReg pickTemp(U32 cls, U64 hard, U64 soft);
		const Interval& bundle(VReg v) const;
		U64 busyAt(U32 i) const;
	private:
		B32 local = false;
		MachineFunc* fn = nullptr;
		const RegisterInfo* ri = nullptr;
		RegAllocHooks hooks;
		U32 nv = 0;
		U64 allocMask[kMaxRegClasses] = {};
		U64 calleeMask = 0;
		// numbering
		List<U32> blockFirst; // block -> first instruction
		List<U64> busy;				// slot -> busy physical registers
		List<U64> chunk;			// 64 slots -> busy anywhere in them
		U64 usedCallee = 0;
		// liveness and bundles
		List<U32> outFirst; // block -> first of its live-out vregs
		List<VReg> outVRegs;
		List<U8> cross;									 // vreg -> lives on the stack (local mode)
		List<Interval> iv;							 // grows only, the first nv are live
		List<Pair<VReg, PhysReg>> temps; // spilled bundle -> its temp, for the current instr
	};
} // namespace rat

#endif
