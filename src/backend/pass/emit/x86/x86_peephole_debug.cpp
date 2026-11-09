#include "pass/emit/x86/x86_peephole.h"

#include "codegen/machine_function.h"

namespace rat {
	// in the low bits right half, -1 when untracked
	I32 X86PeepholePass::DbgTaint::bitOf(const MachineOperand& o) const {
		static_assert(kMaxPhys <= 64, "regs fit word 0");
		if(o.isPhys())
			return o.phys < kMaxPhys ? (I32)o.phys : -1;
		if(o.kind != MachineOperand::Kind::FrameSlot)
			return -1;
		U32 k = slotKey(o.slot, keys);
		return k < keys ? 64 + (I32)k : -1;
	}

	// 0 clean, 1 low bits right, 2 wrong
	U32 X86PeepholePass::DbgTaint::level(const MachineOperand& o) const {
		I32 bit = bitOf(o);
		U32 lv = 0;
		for(U32 p = 0; bit >= 0 && p < 2; ++p, bit += (I32)marks.size() * 32)
			if((marks[bit >> 6] >> (bit & 63)) & 1)
				lv = p + 1;
		return lv;
	}

	void X86PeepholePass::DbgTaint::set(const MachineOperand& o, U32 lv) {
		I32 bit = bitOf(o);
		for(U32 p = 0; bit >= 0 && p < 2; ++p, bit += (I32)marks.size() * 32) {
			U64 mask = (U64)1 << (bit & 63);
			if(lv == p + 1)
				marks[bit >> 6] |= mask;
			else
				marks[bit >> 6] &= ~mask;
		}
	}

	// ops whose low result bits come from the low bits of their inputs only
	B32 X86PeepholePass::keepsLowBits(X86Op op) {
		switch(op) {
		case X86Op::Copy:
		case X86Op::Load:
		case X86Op::Lea:
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::Mul:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor:
		case X86Op::Not:
		case X86Op::Neg:
		case X86Op::Shl:
		case X86Op::CMov:
		case X86Op::MaskBits:
		case X86Op::SignExtBits:
			return true;
		default:
			return false;
		}
	}

	// low taint survives ops that keep low bits and ends at a normalization within the right bits,
	// anything else turns it wrong
	void X86PeepholePass::taintStep(const MachineInstr& in, DbgTaint& t) {
		if(isAnySlotStore(in)) {
			const MachineOperand& slot = in.uses[0];
			U32 kept = in.uses[1].width < 8 ? t.level(slot) : 0;
			t.set(slot, std::max(kept, t.level(in.uses[1])));
			return;
		}
		U32 lv = 0;
		for(const MachineOperand& u : in.uses)
			lv = std::max(lv, t.level(u));
		X86Op op = (X86Op)in.op;
		if(lv == 1 && !keepsLowBits(op))
			lv = 2;
		if(lv == 1 && isNormalize(op) && (U64)in.imm <= t.rightBits)
			lv = 0;
		t.marks[0] &= ~in.clobbers;
		t.marks[t.marks.size() / 2] &= ~in.clobbers;
		for(const MachineOperand& d : in.defs) {
			U32 kept = d.width < 4 ? t.level(d) : 0;
			t.set(d, std::max(lv, kept));
		}
	}

	void X86PeepholePass::killRecords(MachineFunc& mf, U32 chain, const DbgTaint& t) {
		for(U32 r = chain; r; r = mf.dbg[r].next) {
			MachineOperand& loc = mf.dbg[r].loc;
			U32 lv = t.level(loc);
			if(lv == 2 || (lv == 1 && loc.width * 8u > t.rightBits))
				loc = MachineOperand::none(loc.width);
		}
	}

	// one sweep in block order, true when a block entry taint grew
	B32 X86PeepholePass::taintPass(MachineFunc& mf,
																 List<List<U64>>& entry,
																 DbgTaint& t,
																 B32 kill) const {
		B32 grew = false;
		const DbgSeed* seed = dbgSeeds.data();
		for(U32 bi = 0; bi < (U32)mf.blocks.size(); ++bi) {
			B32 clean = std::count(entry[bi].begin(), entry[bi].end(), 0) == (I64)entry[bi].size();
			if(clean && seed->block != bi)
				continue; // no taint enters or starts here
			MachineBlock& b = mf.blocks[bi];
			t.marks = entry[bi];
			for(U32 i = 0; i <= (U32)b.insts.size(); ++i) {
				for(; seed->block == bi && seed->at == i; ++seed)
					t.set(seed->loc, seed->bits ? 1u : 2u); // a dead store leaves its slot wrong
				if(kill)
					killRecords(mf, b.dbgBefore(i), t);
				if(i < b.insts.size())
					taintStep(b.insts[i], t);
			}
			for(I32 s : b.succs)
				grew |= orInto(entry[(U32)s].data(), t.marks);
		}
		return grew;
	}

	// forward may-dataflow from the seeds of the phase just run
	void X86PeepholePass::killTainted(MachineFunc& mf) {
		if(dbgSeeds.empty())
			return;
		DbgTaint t;
		t.keys = mf.frameBytes / 8 + 1;
		for(const DbgSeed& s : dbgSeeds)
			if(s.bits)
				t.rightBits = std::min(t.rightBits, s.bits);
		dbgSeeds.push_back({~0u, 0, {}, 0}); // ends the seeds
		List<List<U64>> entry(mf.blocks.size(), List<U64>(2 * (1 + (t.keys + 63) / 64), 0));
		B32 grew = true;
		while(grew)
			grew = taintPass(mf, entry, t, false);
		taintPass(mf, entry, t, true);
		dbgSeeds.clear();
	}
} // namespace rat
