#include "pass/emit/x86/x86_peephole.h"

#include "codegen/machine_function.h"

namespace rat {
	U32 X86PeepholePass::slotKey(I32 s) const {
		U32 last = (U32)slots.size() - 1;
		if(s >= 0 || (s & 7))
			return last;
		return std::min((U32)(-(I64)s >> 3), last);
	}

	// defs[0] = register, uses[0] = frame slot
	B32 X86PeepholePass::isAnySlotLoad(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		if((op != X86Op::Load && op != X86Op::FLoad) || in.defs.size() != 1 || in.uses.size() != 1)
			return false;
		return in.defs[0].isPhys() && in.uses[0].isFrameSlot();
	}

	U32 X86PeepholePass::regValue(PhysReg p) const { return p < kMaxPhys ? reg[p] : 0; }

	void X86PeepholePass::setReg(PhysReg p, U32 v) {
		if(p < kMaxPhys)
			reg[p] = v;
	}

	U32 X86PeepholePass::slotValue(I32 s, U32 width) const {
		U32 k = slotKey(s);
		if(k + 1 == slots.size() || slots[k].value < validFrom || slots[k].width != width)
			return 0;
		return slots[k].value;
	}

	void X86PeepholePass::setSlot(I32 s, U32 width, U32 v) {
		U32 k = slotKey(s);
		if(k + 1 < slots.size()) { // the shared slot never holds a value
			slots[k].value = v;
			slots[k].width = width;
		}
	}

	void X86PeepholePass::killAllRegs() {
		for(U32& v : reg)
			v = nextValue++;
	}

	void X86PeepholePass::forgetAll() {
		validFrom = nextValue;
		killAllRegs();
	}

	// both classes copy the full register, so the value carries exactly
	X86PeepholePass::Fold X86PeepholePass::foldCopy(const MachineInstr& in) {
		PhysReg d = in.defs[0].phys;
		PhysReg s = in.uses[0].phys;
		if(d == s || regValue(d) == regValue(s))
			return Fold::Dropped;
		setReg(d, regValue(s));
		return Fold::None;
	}

	// makeSpill shape, uses[0] = frame slot, uses[1] = source register
	X86PeepholePass::Fold X86PeepholePass::foldSpill(const MachineInstr& in) {
		I32 slot = in.uses[0].slot;
		U32 width = in.uses[1].width;
		U32 v = regValue(in.uses[1].phys);
		if(slotValue(slot, width) == v) // memory already has it
			return Fold::Dropped;
		setSlot(slot, width, v);
		return Fold::None;
	}

	// makeReload shape, defs[0] = register, uses[0] = frame slot
	X86PeepholePass::Fold X86PeepholePass::foldReload(MachineInstr& in) {
		I32 slot = in.uses[0].slot;
		U32 width = in.defs[0].width;
		PhysReg d = in.defs[0].phys;
		U32 v = slotValue(slot, width);
		if(v == 0) {
			v = nextValue++;
			setReg(d, v);
			setSlot(slot, width, v);
			return Fold::None;
		}
		if(regValue(d) == v) // the register already has it
			return Fold::Dropped;
		PhysReg src = kNoReg;
		for(PhysReg p = 1; p < kMaxPhys && src == kNoReg; ++p)
			if(reg[p] == v && p != d)
				src = p;
		setReg(d, v);
		if(src == kNoReg)
			return Fold::None;
		// a copy replaces the reload, keeping its debug location and records
		MachineInstr copy;
		copy.op = (MachineOpcode)X86Op::Copy;
		copy.loc = in.loc;
		copy.dbg = in.dbg;
		copy.defs = {MachineOperand::fixed(d, width)};
		copy.uses = {MachineOperand::fixed(src, width)};
		in = std::move(copy);
		return Fold::Rewritten;
	}

	// folds in against the value numbers, any other instruction steps them past it
	X86PeepholePass::Fold X86PeepholePass::fold(MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		B32 copy = op == X86Op::Copy && in.defs.size() == 1 && in.uses.size() == 1;
		if(copy && in.defs[0].isPhys() && in.uses[0].isPhys())
			return foldCopy(in);
		if(op == X86Op::Store && detail::isAnySlotStore(in))
			return foldSpill(in);
		if(op == X86Op::Load && isAnySlotLoad(in))
			return foldReload(in);
		B32 writesSlot = false; // other than through the spill shape
		for(const MachineOperand& d : in.defs)
			writesSlot |= d.isFrameSlot();
		if(!x86OpInfo(op).transparent || writesSlot) {
			forgetAll();
			return Fold::None;
		}
		if(op == X86Op::Call)
			killAllRegs(); // hidden scratch use in the argument shuffle
		for(const MachineOperand& d : in.defs)
			if(d.isPhys())
				setReg(d.phys, nextValue++);
		for(U64 m = in.clobbers; m; m &= m - 1)
			setReg((PhysReg)countTrailingZeros64(m), nextValue++);
		return Fold::None;
	}

	// also notes the slot accesses of the kept instructions
	U32 X86PeepholePass::foldBlock(MachineFunc& mf, MachineBlock& b, List<B32>& drop) {
		U32 changed = 0;
		forgetAll();
		drop.assign(b.insts.size(), false);
		for(U32 i = 0; i < (U32)b.insts.size(); ++i) {
			MachineInstr& in = b.insts[i];
			Fold folded = fold(in);
			if(folded != Fold::Dropped && noteSlots(in))
				touches[(U32)b.id] = true;
			if(folded == Fold::None)
				continue;
			++changed;
			drop[i] = folded == Fold::Dropped;
		}
		if(changed)
			detail::eraseMarked(mf, b, drop, false);
		return changed;
	}

	namespace detail {
		// drop the flagged instructions, their debug records move to the next kept one.
		// kill also kills the slot each dropped store leaves stale
		void eraseMarked(MachineFunc& mf, MachineBlock& b, const List<B32>& drop, B32 kill) {
			U32 kept = 0;
			U32 dbg = 0;
			for(U32 i = 0; i < (U32)b.insts.size(); ++i) {
				MachineInstr& in = b.insts[i];
				if(drop[i]) {
					dbg = mf.joinDbg(dbg, in.dbg);
					if(kill)
						mf.addDbg(dbg, kDbgKill, in.uses[0]);
					continue;
				}
				in.dbg = mf.joinDbg(dbg, in.dbg);
				dbg = 0;
				if(kept != i)
					b.insts[kept] = std::move(in);
				++kept;
			}
			b.dbgEnd = mf.joinDbg(dbg, b.dbgEnd);
			b.insts.erase(b.insts.begin() + kept, b.insts.end());
		}
	} // namespace detail

	// backward fixpoint of the live slots at block entry.
	// any visit order reaches it, the last blocks first is just faster
	void X86PeepholePass::solve(const MachineFunc& mf, List<U64>& liveIn, List<U64>& cur) {
		List<U32> work;
		List<B32> queued(mf.blocks.size(), false);
		for(U32 bi = 0; bi < (U32)mf.blocks.size(); ++bi)
			if(mf.blocks[bi].id >= 0) {
				work.push_back(bi);
				queued[bi] = true;
			}
		while(!work.empty()) {
			U32 bi = work.back();
			work.pop_back();
			queued[bi] = false;
			const MachineBlock& b = mf.blocks[bi];
			blockOut(b, liveIn, cur);
			if(touches[bi])
				for(U32 i = (U32)b.insts.size(); i-- > 0;)
					slotStep(b.insts[i], cur);
			U64* in = &liveIn[(U64)bi * cur.size()];
			U64 grew = 0;
			for(U32 i = 0; i < (U32)cur.size(); ++i) {
				grew |= cur[i] & ~in[i];
				in[i] |= cur[i];
			}
			if(!grew)
				continue;
			for(I32 p : b.preds) // a grown live-in reaches the predecessors
				if(!queued[(U32)p]) {
					work.push_back((U32)p);
					queued[(U32)p] = true;
				}
		}
	}

	// union of successor live-ins, everything for open blocks
	// inline, called per block in the fixpoint
	inline void
	X86PeepholePass::blockOut(const MachineBlock& b, const List<U64>& liveIn, List<U64>& cur) {
		B32 open = false;
		if(b.succs.empty() && !b.insts.empty()) {
			X86Op last = (X86Op)b.insts.back().op;
			open = last != X86Op::Ret && last != X86Op::Ud2;
		}
		U64 stride = cur.size();
		U64* out = cur.data();
		std::fill(out, out + stride, open ? kAllBits : 0);
		for(I32 s : b.succs)
			if(s >= 0 && (U64)s * stride < liveIn.size()) {
				const U64* in = liveIn.data() + (U64)s * stride;
				for(U64 i = 0; i < stride; ++i)
					out[i] |= in[i];
			}
	}

	// a slot is tracked while it is only touched through the spill store and
	// reload shapes plus call stack arguments. returns whether in reaches a slot that way
	B32 X86PeepholePass::noteSlots(const MachineInstr& in) {
		B32 load = isAnySlotLoad(in);
		if(load || detail::isAnySlotStore(in)) {
			Slot& slot = slots[slotKey(in.uses[0].slot)];
			slot.seen = true;
			if(load)
				slot.readWidth = std::max(slot.readWidth, (U32)in.defs[0].width);
			return true;
		}
		B32 reaches = false;
		for(const MachineOperand& u : in.uses)
			if(u.isFrameSlot()) {
				Slot& slot = slots[slotKey(u.slot)];
				if(in.isCall) {
					slot.seen = true;
					slot.readWidth = std::max(slot.readWidth, (U32)u.width);
					reaches = true;
				} else {
					slot.untracked = true;
				}
			}
		for(const MachineOperand& d : in.defs)
			if(d.isFrameSlot())
				slots[slotKey(d.slot)].untracked = true;
		return reaches;
	}

	// walk one instruction backwards over the tracked slot liveness bits
	void X86PeepholePass::slotStep(const MachineInstr& in, List<U64>& cur) const {
		if(detail::isAnySlotStore(in)) {
			const Slot& slot = slots[slotKey(in.uses[0].slot)];
			if(slot.bit != kNoBit && in.uses[1].width >= slot.readWidth)
				cur[slot.bit >> 6] &= ~((U64)1 << (slot.bit & 63)); // full overwrite
			return;
		}
		if(!isAnySlotLoad(in) && !in.isCall)
			return;
		for(const MachineOperand& u : in.uses)
			if(u.isFrameSlot())
				if(U32 bit = slots[slotKey(u.slot)].bit; bit != kNoBit)
					cur[bit >> 6] |= (U64)1 << (bit & 63);
	}

	U32 X86PeepholePass::elimDeadSlotStores(MachineFunc& mf) {
		U32 count = 0;
		for(U32 k = 0; k + 1 < (U32)slots.size(); ++k)
			if(slots[k].seen && !slots[k].untracked)
				slots[k].bit = count++;
		if(!count)
			return 0;

		U32 nb = (U32)mf.blocks.size();
		U32 words = (count + 63) / 64;
		List<U64> liveIn((U64)nb * words, 0); // block -> live slot bits at entry
		List<U64> cur(words, 0);
		solve(mf, liveIn, cur);

		U32 removed = 0;
		List<B32> drop;
		for(U32 bi = 0; bi < nb; ++bi) {
			MachineBlock& b = mf.blocks[bi];
			if(!touches[bi])
				continue;
			blockOut(b, liveIn, cur);
			U32 dead = markDeadStores(b, cur, drop);
			if(dead)
				detail::eraseMarked(mf, b, drop, mf.dbg.size() > 1);
			removed += dead;
		}
		return removed;
	}

	// walk b backwards from its live-out in cur and mark the stores nobody reads
	U32 X86PeepholePass::markDeadStores(const MachineBlock& b, List<U64>& cur, List<B32>& drop) {
		drop.assign(b.insts.size(), false);
		U32 dead = 0;
		for(U32 i = (U32)b.insts.size(); i-- > 0;) {
			const MachineInstr& in = b.insts[i];
			U32 bit = detail::isAnySlotStore(in) ? slots[slotKey(in.uses[0].slot)].bit : kNoBit;
			if(bit != kNoBit && !((cur[bit >> 6] >> (bit & 63)) & 1)) {
				drop[i] = true;
				++dead;
				continue;
			}
			slotStep(in, cur);
		}
		return dead;
	}

	B32 X86PeepholePass::run(Module&, const Function&, MachineFunc& mf, const TargetInfo&) {
		U32 changed = 0;
		slots.assign(mf.frameBytes / 8 + 2, {});
		touches.assign(mf.blocks.size(), false);
		List<B32> drop; // per instruction of the swept block
		for(MachineBlock& b : mf.blocks)
			if(b.id >= 0)
				changed += foldBlock(mf, b, drop);
		changed += elimDeadSlotStores(mf);
		return changed != 0;
	}
} // namespace rat
