#ifndef RAT_CODEGEN_REGALLOCBASE_H
#define RAT_CODEGEN_REGALLOCBASE_H

#include "Core.h"

#include "CodeGen/MachineFunction.h"
#include "Support/Pass.h"

namespace rat {
	// dense bitset over vreg numbers
	struct VRegSet {
		explicit VRegSet(U32 bits = 0)
		: words((bits + 63) / 64, 0) {}

		B32 test(VReg v) const { return (U32)((words[v >> 6] >> (v & 63)) & 1); }
		void set(VReg v) { words[v >> 6] |= (U64)1 << (v & 63); }
		void reset(VReg v) { words[v >> 6] &= ~((U64)1 << (v & 63)); }

		B32 operator==(const VRegSet& o) const { return words == o.words; }

		void orWith(const VRegSet& o) {
			for(U32 i = 0; i < (U32)words.size(); ++i)
				words[i] |= o.words[i];
		}

		// this = a | (b & ~mask)
		void assignUnionMasked(const VRegSet& a, const VRegSet& b, const VRegSet& mask) {
			for(U32 i = 0; i < (U32)words.size(); ++i)
				words[i] = a.words[i] | (b.words[i] & ~mask.words[i]);
		}

		template <typename F> void forEach(F f) const { // ascending vreg order
			for(U32 wi = 0; wi < (U32)words.size(); ++wi)
				for(U64 w = words[wi]; w; w &= w - 1)
					f((VReg)(wi * 64 + (U32)__builtin_ctzll(w)));
		}
	private:
		List<U64> words;
	};

	struct RegAllocBase : MachinePass {
		B32 run(Module& module, MachineModule& mm, const TargetInfo& target) override;
	protected:
		struct Loc {
			U32 block;
			U32 inst;
		};

		// where a vreg ended up: a physical register, or a frame slot when spilled
		struct Assignment {
			PhysReg reg = kNoReg;
			I32 spillSlot = 0;
			U32 cls = 0;
			B32 spilled = false;
		};

		// one side of a coalescable copy and the point of the copy itself
		struct CopyHint {
			VReg partner;
			I32 pt;
		};

		virtual void resetState() = 0;							 // clear solver state between functions
		virtual void solve() = 0;										 // compute assignments (number() already ran)
		virtual Assignment assignmentOf(VReg v) = 0; // result lookup used by rewrite()

		B32 allocate(MachineFunc& fn,
								 const RegisterInfo& ri,
								 const RegAllocHooks& hooks,
								 List<PhysReg>* usedCalleeSaved);

		void number();
		void pinFixedArgWindows();
		void collectCopyHints();
		void liveness(List<VRegSet>& liveIn, List<VRegSet>& liveOut);

		// allocation preferences derived from copies
		List<PhysReg> hintedRegs(VReg v, const Delegate<PhysReg(VReg)>& colorOf) const;

		// program points of copies connecting two move partners
		List<I32> copyPointsBetween(VReg a, VReg b) const;

		// whether the pin of p at pt is a hint copy for v
		B32 pinExempt(VReg v, I32 pt, PhysReg p) const;

		// spill-slot pool
		I32 takeSpillSlot(U32 cls, I32 start, I32 end);
		U32 classOf(VReg v) const;
		const RegClass& regClass(U32 cls) const;
		static B32 isCalleeSaved(const RegClass& rc, PhysReg p);
		static B32 isAllocatable(const RegClass& rc, PhysReg p);
		void rewrite();
		PhysReg scratchAt(U32 cls, U32 idx);
	protected:
		MachineFunc* fn = nullptr;
		const RegisterInfo* ri = nullptr;
		const RegAllocHooks* hooks = nullptr;
		List<Loc> order;								// linear point -> (block, instruction)
		List<List<U32>> blkPts;					// block -> its linear points
		List<I32> callPts;							// points that are calls
		Map<I32, Set<PhysReg>> fixedAt; // physical registers pinned at a point
		Set<PhysReg> usedCallee;
		Map<VReg, List<CopyHint>> copyHints;		// vreg <-> vreg move affinities
		Map<VReg, List<PhysReg>> physHints;			// vreg <-> fixed-register move affinities
		Map<VReg, Map<I32, PhysReg>> copyPinAt; // pin exemptions
		B32 ok = true;
	private:
		struct PooledSlot {
			I32 slot;
			I32 freeEnd; // last point of the current occupant's live range
		};
		Map<U32, List<PooledSlot>> slotPool; // per register class
	};
} // namespace rat

#endif
