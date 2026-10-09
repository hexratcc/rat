#include "pass/emit/x86/x86_peephole.h"

#include "codegen/machine_function.h"

namespace rat {
	U32 X86PeepholePass::slotKey(I32 s, U32 keys) {
		if(s >= 0 || (s & 7))
			return keys;
		U32 k = (U32)(-(I64)s >> 3);
		if(k >= keys)
			return keys;
		return k;
	}

	void X86PeepholePass::ValueState::begin(U32 frameKeys) {
		keys = frameKeys;
		slot.assign(keys, {});
	}

	void X86PeepholePass::ValueState::reset() {
		killAllRegs();
		++epoch;
	}

	void X86PeepholePass::ValueState::killAllRegs() {
		for(U32 i = 0; i < kMaxPhys; ++i)
			reg[i] = fresh();
	}

	void X86PeepholePass::ValueState::killReg(PhysReg p) {
		if(p < kMaxPhys)
			reg[p] = fresh();
	}

	void X86PeepholePass::ValueState::setReg(PhysReg p, U32 v) {
		if(p < kMaxPhys)
			reg[p] = v;
	}

	PhysReg X86PeepholePass::ValueState::regHolding(U32 v, PhysReg except) const {
		for(U32 i = 1; i < kMaxPhys; ++i)
			if(reg[i] == v && (PhysReg)i != except)
				return (PhysReg)i;
		return kNoReg;
	}

	U32 X86PeepholePass::ValueState::slotValue(I32 s, U32 width) const {
		U32 k = slotKey(s, keys);
		if(k == keys || slot[k].epoch != epoch || slot[k].width != width)
			return 0;
		return slot[k].value;
	}

	void X86PeepholePass::ValueState::setSlot(I32 s, U32 width, U32 v) {
		U32 k = slotKey(s, keys);
		if(k < keys) // an untracked slot never holds a value
			slot[k] = {v, width, epoch};
	}

	// an instruction that reaches frame memory outside its operands, or that can
	// be re-entered, invalidates everything tracked
	B32 X86PeepholePass::isTransparent(X86Op op) {
		switch(op) {
		case X86Op::Copy:
		case X86Op::LoadImm:
		case X86Op::LoadSym:
		case X86Op::FrameAddr:
		case X86Op::RetAddr:
		case X86Op::Lea:
		case X86Op::Load:
		case X86Op::Store:
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::Mul:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor:
		case X86Op::Neg:
		case X86Op::Not:
		case X86Op::Shl:
		case X86Op::AShr:
		case X86Op::LShr:
		case X86Op::Rotl:
		case X86Op::Rotr:
		case X86Op::BitScanF:
		case X86Op::BitScanR:
		case X86Op::Cmp:
		case X86Op::SetCC:
		case X86Op::CMov:
		case X86Op::MaskBits:
		case X86Op::SignExtBits:
		case X86Op::Bswap:
		case X86Op::FLoad:
		case X86Op::FStore:
		case X86Op::FAdd:
		case X86Op::FSub:
		case X86Op::FMul:
		case X86Op::FDiv:
		case X86Op::FNeg:
		case X86Op::FSqrt:
		case X86Op::FAbs:
		case X86Op::FCmp:
		case X86Op::FCmpFlags:
		case X86Op::Call: // clobbers cover the volatile set, slots are below rsp
		case X86Op::Jmp:
		case X86Op::Br:
		case X86Op::Ret:
		case X86Op::SwitchJump:
			return true;
		default:
			return false;
		}
	}

	B32 X86PeepholePass::isRegCopy(const MachineInstr& in) {
		return (X86Op)in.op == X86Op::Copy && in.defs.size() == 1 && in.uses.size() == 1 &&
					 in.defs[0].isPhys() && in.uses[0].isPhys();
	}

	// makeSpill: uses[0] = frame slot, uses[1] = source register
	B32 X86PeepholePass::isSlotStore(const MachineInstr& in) {
		return (X86Op)in.op == X86Op::Store && in.regClass == detail::kGp && isAnySlotStore(in);
	}

	// makeReload: defs[0] = register, uses[0] = frame slot
	B32 X86PeepholePass::isSlotLoad(const MachineInstr& in) {
		return (X86Op)in.op == X86Op::Load && in.regClass == detail::kGp && isAnySlotLoad(in);
	}

	MachineInstr X86PeepholePass::makeCopy(PhysReg dst, PhysReg src, U32 cls, U32 width) {
		MachineInstr m;
		m.op = (MachineOpcode)X86Op::Copy;
		m.regClass = cls;
		m.defs = {MachineOperand::fixed(dst, width)};
		m.uses = {MachineOperand::fixed(src, width)};
		return m;
	}

	// a frame slot written by anything other than the recognized spill shape
	B32 X86PeepholePass::writesUntrackedSlot(const MachineInstr& in) {
		for(const MachineOperand& d : in.defs)
			if(d.kind == MachineOperand::Kind::FrameSlot)
				return true;
		return false;
	}

	U32 X86PeepholePass::foldRegCopy(MachineInstr& in, B32& keep) {
		PhysReg d = in.defs[0].phys;
		PhysReg s = in.uses[0].phys;
		// both classes copy the full register, so the value carries exactly
		if(d == s || st.valueOf(d) == st.valueOf(s)) {
			keep = false;
			return 1;
		}
		st.setReg(d, st.valueOf(s));
		return 0;
	}

	U32 X86PeepholePass::foldSlotStore(MachineInstr& in, B32& keep) {
		I32 sl = in.uses[0].slot;
		U32 w = in.uses[1].width;
		U32 v = st.valueOf(in.uses[1].phys);
		if(st.slotValue(sl, w) == v) { // memory already has it
			keep = false;
			return 1;
		}
		st.setSlot(sl, w, v);
		return 0;
	}

	U32 X86PeepholePass::foldSlotLoad(MachineInstr& in, B32& keep) {
		I32 sl = in.uses[0].slot;
		U32 w = in.defs[0].width;
		PhysReg d = in.defs[0].phys;
		U32 v = st.slotValue(sl, w);
		if(v != 0) {
			if(st.valueOf(d) == v) { // the register already has it
				keep = false;
				return 1;
			}
			if(PhysReg src = st.regHolding(v, d); src != kNoReg) {
				in = makeCopy(d, src, in.regClass, w);
				st.setReg(d, v);
				return 1;
			}
			st.setReg(d, v);
		} else {
			v = st.fresh();
			st.setReg(d, v);
			st.setSlot(sl, w, v);
		}
		return 0;
	}

	void X86PeepholePass::stepOther(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		if(!isTransparent(op) || writesUntrackedSlot(in)) {
			st.reset();
			return;
		}
		if(op == X86Op::Call)
			st.killAllRegs(); // hidden scratch use in the argument shuffle
		for(const MachineOperand& d : in.defs)
			if(d.isPhys())
				st.killReg(d.phys);
		for(U64 m = in.clobbers; m; m &= m - 1)
			st.killReg((PhysReg)countTrailingZeros64(m));
	}

	U32 X86PeepholePass::runOnBlock(MachineBlock& b) {
		U32 changed = 0;
		U32 kept = 0; // compacts in place, a fold never adds instructions
		st.reset();
		for(U32 i = 0; i < (U32)b.insts.size(); ++i) {
			MachineInstr& in = b.insts[i];
			B32 keep = true;
			if(isRegCopy(in))
				changed += foldRegCopy(in, keep);
			else if(isSlotStore(in))
				changed += foldSlotStore(in, keep);
			else if(isSlotLoad(in))
				changed += foldSlotLoad(in, keep);
			else
				stepOther(in);
			if(!keep)
				continue;
			slots.note(in, (U32)b.id);
			if(kept != i)
				b.insts[kept] = std::move(in);
			++kept;
		}
		b.insts.erase(b.insts.begin() + kept, b.insts.end());
		return changed;
	}

	// TODO(hexratcc): hack, impl i32 ops

	U64 X86PeepholePass::lowMask(U32 n) { return n >= 64 ? kAllBits : (((U64)1 << n) - 1); }

	// carries only travel upward, so a demand up to bit h reads bits [0,h]
	U64 X86PeepholePass::carryMask(U64 out) {
		if(!out)
			return 0;
		return lowMask((U32)(64 - countLeadingZeros64(out)));
	}

	B32 X86PeepholePass::isNormalize(X86Op op) {
		return op == X86Op::MaskBits || op == X86Op::SignExtBits;
	}

	void X86PeepholePass::demandUses(const MachineInstr& in, U64 mask, U64* dem, U32 from, U32 to) {
		U32 end = std::min(to, (U32)in.uses.size());
		for(U32 i = from; i < end; ++i)
			if(tracked(in.uses[i]))
				dem[in.uses[i].phys] |= mask;
	}

	B32 X86PeepholePass::immCount(const MachineInstr& in, U32& out) {
		if(in.uses.size() < 2 || in.uses[1].kind != MachineOperand::Kind::Imm)
			return false;
		out = (U32)(in.uses[1].imm & 63);
		return true;
	}

	B32 X86PeepholePass::readsFlags(X86Op op) {
		return op == X86Op::SetCC || op == X86Op::CMov || op == X86Op::Br;
	}

	B32 X86PeepholePass::writesFlags(X86Op op) {
		switch(op) {
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::Mul:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor:
		case X86Op::Neg:
		case X86Op::Shl:
		case X86Op::AShr:
		case X86Op::LShr:
		case X86Op::Rotl:
		case X86Op::Rotr:
		case X86Op::Cmp:
		case X86Op::FCmp:
		case X86Op::FCmpFlags:
		case X86Op::MaskBits:
		case X86Op::BitScanF:
		case X86Op::BitScanR:
		case X86Op::SDiv:
		case X86Op::SRem:
		case X86Op::UDiv:
		case X86Op::URem:
		case X86Op::Call:
			return true;
		default:
			return false;
		}
	}

	// MaskBits and wide SignExtBits set flags, so dropping one is only safe when
	// the next instruction to care about flags overwrites them anyway
	B32 X86PeepholePass::flagSafeToDrop(const MachineBlock& b, U32 at) {
		if((X86Op)b.insts[at].op == X86Op::SignExtBits && b.insts[at].imm == 32)
			return true; // movsxd leaves flags alone
		for(U32 i = at + 1; i < (U32)b.insts.size(); ++i) {
			X86Op op = (X86Op)b.insts[i].op;
			if(readsFlags(op))
				return false;
			if(writesFlags(op))
				return true;
		}
		return true;
	}

	B32 X86PeepholePass::hasNormalize(const MachineBlock& b) {
		for(const MachineInstr& in : b.insts)
			if(isNormalize((X86Op)in.op))
				return true;
		return false;
	}

	// walk one instruction backwards, dem holds the demand after it on entry
	void X86PeepholePass::transfer(const MachineInstr& in, U64* dem) {
		X86Op op = (X86Op)in.op;
		U64 outD = 0;
		if(!in.defs.empty() && in.defs[0].isPhys())
			outD = tracked(in.defs[0]) ? dem[in.defs[0].phys] : kAllBits;
		for(const MachineOperand& d : in.defs)
			if(tracked(d))
				dem[d.phys] = 0;
		for(U64 m = in.clobbers & lowMask(kDemRegs); m; m &= m - 1)
			dem[countTrailingZeros64(m)] = 0;

		U32 cnt = 0;
		switch(op) {
		case X86Op::SignExtBits: {
			U32 n = (U32)in.imm;
			U64 m = outD & lowMask(n);
			if(n > 0 && n < 64 && (outD & ~lowMask(n)))
				m |= (U64)1 << (n - 1); // the copied sign bit
			demandUses(in, m, dem);
			return;
		}
		case X86Op::MaskBits:
			demandUses(in, outD & lowMask((U32)in.imm), dem);
			return;
		case X86Op::Copy:
		case X86Op::CMov:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor:
		case X86Op::Not:
			demandUses(in, outD, dem);
			return;
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::Mul:
		case X86Op::Neg:
			demandUses(in, carryMask(outD), dem);
			return;
		case X86Op::Shl:
			if(immCount(in, cnt)) {
				demandUses(in, outD >> cnt, dem);
				return;
			}
			break;
		case X86Op::LShr:
			if(immCount(in, cnt)) {
				demandUses(in, cnt ? (outD << cnt) : outD, dem);
				return;
			}
			break;
		case X86Op::AShr:
			if(immCount(in, cnt)) {
				U64 m = cnt ? (outD << cnt) : outD;
				if(cnt && (outD >> (64 - cnt)))
					m |= (U64)1 << 63; // the replicated sign bit
				demandUses(in, m, dem);
				return;
			}
			break;
		case X86Op::Store:
			// a frame slot is always written full width, a real store is not
			if(in.uses.size() >= 2 && in.uses[0].kind != MachineOperand::Kind::FrameSlot) {
				demandUses(in, kAllBits, dem, 0, 1); // address
				demandUses(in, kAllBits, dem, 2);		 // index
				if(tracked(in.uses[1]))							 // stored value
					dem[in.uses[1].phys] |= lowMask(in.uses[1].width * 8);
				return;
			}
			break;
		default:
			break;
		}
		demandUses(in, kAllBits, dem);
	}

	// erase normalizations whose high bits nobody reads
	U32 X86PeepholePass::elimRedundantExt(MachineFunc& mf) {
		U32 nb = (U32)mf.blocks.size();
		List<U8> norm(nb, 0);
		B32 any = false;
		for(U32 bi = 0; bi < nb; ++bi) {
			norm[bi] = (U8)(mf.blocks[bi].id >= 0 && hasNormalize(mf.blocks[bi]));
			any |= norm[bi];
		}
		if(!any)
			return 0;

		List<U64> demIn((U64)nb * kDemRegs, 0);
		List<U64> cur(kDemRegs, 0);

		List<U32> work;
		List<B32> queued;
		seedWork(mf, work, queued);
		while(!work.empty()) {
			U32 bi = work.back();
			work.pop_back();
			queued[bi] = false;
			const MachineBlock& b = mf.blocks[bi];
			slotBlockOut(b, demIn, cur);
			for(U32 i = (U32)b.insts.size(); i-- > 0;)
				transfer(b.insts[i], cur.data());
			if(orInto(&demIn[(U64)bi * kDemRegs], cur))
				queuePreds(b, work, queued);
		}

		U32 dropped = 0;
		for(U32 bi = 0; bi < nb; ++bi) {
			MachineBlock& b = mf.blocks[bi];
			if(!norm[bi])
				continue;
			slotBlockOut(b, demIn, cur);

			drop.assign(b.insts.size(), false);
			U32 here = 0;
			for(U32 i = (U32)b.insts.size(); i-- > 0;) {
				const MachineInstr& in = b.insts[i];
				X86Op op = (X86Op)in.op;
				U32 n = (U32)in.imm;
				if(isNormalize(op) && n > 0 && n < 64 && !in.defs.empty() && tracked(in.defs[0]) &&
					 !(cur[in.defs[0].phys] & ~lowMask(n)) && flagSafeToDrop(b, i)) {
					drop[i] = true;
					++here;
					continue; // dead
				}
				transfer(in, cur.data());
			}
			dropped += here;
			if(here)
				eraseMarked(b, drop);
		}
		return dropped;
	}

	// drop the flagged instructions
	void X86PeepholePass::eraseMarked(MachineBlock& b, const List<B32>& drop) {
		U32 kept = 0;
		for(U32 i = 0; i < (U32)b.insts.size(); ++i) {
			if(drop[i])
				continue;
			if(kept != i)
				b.insts[kept] = std::move(b.insts[i]);
			++kept;
		}
		b.insts.erase(b.insts.begin() + kept, b.insts.end());
	}

	B32 X86PeepholePass::orInto(U64* into, const List<U64>& from) {
		U64 grew = 0;
		for(U32 i = 0; i < (U32)from.size(); ++i) {
			grew |= from[i] & ~into[i];
			into[i] |= from[i];
		}
		return grew != 0;
	}

	void X86PeepholePass::seedWork(const MachineFunc& mf, List<U32>& work, List<B32>& queued) {
		if(seedQueued.empty())
			postorder(mf);
		work.assign(seed.rbegin(), seed.rend());
		queued = seedQueued;
	}

	void X86PeepholePass::postorder(const MachineFunc& mf) {
		List<B32>& queued = seedQueued;
		List<U32>& post = seed;
		post.clear();
		queued.assign(mf.blocks.size(), false);
		List<std::pair<U32, U32>> stack; // block, next successor
		for(U32 root = 0; root < (U32)mf.blocks.size(); ++root) {
			if(mf.blocks[root].id < 0 || queued[root])
				continue;
			queued[root] = true;
			stack.push_back({root, 0});
			while(!stack.empty()) {
				auto& [bi, next] = stack.back();
				const SmallList<I32, 2>& succs = mf.blocks[bi].succs;
				if(next == (U32)succs.size()) {
					post.push_back(bi);
					stack.pop_back();
					continue;
				}
				U32 s = (U32)succs[next++];
				if(!queued[s]) {
					queued[s] = true;
					stack.push_back({s, 0});
				}
			}
		}
	}

	// a grown live-in reaches the predecessors
	void X86PeepholePass::queuePreds(const MachineBlock& b, List<U32>& work, List<B32>& queued) {
		for(I32 p : b.preds)
			if(!queued[(U32)p]) {
				work.push_back((U32)p);
				queued[(U32)p] = true;
			}
	}

	// uses[0] = frame slot, uses[1] = source
	B32 X86PeepholePass::isAnySlotStore(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		return (op == X86Op::Store || op == X86Op::FStore) && in.defs.empty() && in.uses.size() == 2 &&
					 in.uses[0].kind == MachineOperand::Kind::FrameSlot && in.uses[1].isPhys();
	}

	// defs[0] = register, uses[0] = frame slot
	B32 X86PeepholePass::isAnySlotLoad(const MachineInstr& in) {
		X86Op op = (X86Op)in.op;
		return (op == X86Op::Load || op == X86Op::FLoad) && in.defs.size() == 1 &&
					 in.defs[0].isPhys() && in.uses.size() == 1 &&
					 in.uses[0].kind == MachineOperand::Kind::FrameSlot;
	}

	// union of successor live-ins, everything for open blocks
	// inline: called per block in the demand fixpoints
	inline void
	X86PeepholePass::slotBlockOut(const MachineBlock& b, const List<U64>& liveIn, List<U64>& cur) {
		B32 open = b.succs.empty() && !b.insts.empty() && (X86Op)b.insts.back().op != X86Op::Ret &&
							 (X86Op)b.insts.back().op != X86Op::Ud2;
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

	U32 X86PeepholePass::TrackedSlots::key(I32 s) const { return slotKey(s, keys); }

	// walk one instruction backwards over the tracked-slot liveness bits
	void
	X86PeepholePass::slotStep(const MachineInstr& in, const TrackedSlots& slots, List<U64>& cur) {
		if(isAnySlotStore(in)) {
			U32 k = slots.key(in.uses[0].slot);
			U32 bit = slots.index[k];
			if(bit != kNoBit && in.uses[1].width >= slots.readWidth[k])
				cur[bit >> 6] &= ~((U64)1 << (bit & 63)); // full overwrite
			return;
		}
		if(isAnySlotLoad(in)) {
			U32 bit = slots.index[slots.key(in.uses[0].slot)];
			if(bit != kNoBit)
				cur[bit >> 6] |= (U64)1 << (bit & 63);
			return;
		}
		if(in.isCall)
			for(const MachineOperand& u : in.uses)
				if(u.kind == MachineOperand::Kind::FrameSlot)
					if(U32 bit = slots.index[slots.key(u.slot)]; bit != kNoBit)
						cur[bit >> 6] |= (U64)1 << (bit & 63);
	}

	// a slot is tracked while it is only touched through the spill store and
	// reload shapes plus call stack arguments
	void X86PeepholePass::TrackedSlots::begin(const MachineFunc& mf) {
		keys = mf.frameBytes / 8 + 1;
		count = 0;
		seen.assign(keys + 1, false);
		untracked.assign(keys + 1, false);
		index.assign(keys + 1, kNoBit);
		readWidth.assign(keys + 1, 0);
		touches.assign(mf.blocks.size(), false);
	}

	void X86PeepholePass::TrackedSlots::note(const MachineInstr& in, U32 block) {
		if(isAnySlotStore(in)) {
			seen[key(in.uses[0].slot)] = true;
			touches[block] = true;
			return;
		}
		if(isAnySlotLoad(in)) {
			U32 k = key(in.uses[0].slot);
			touches[block] = true;
			seen[k] = true;
			readWidth[k] = std::max(readWidth[k], (U32)in.defs[0].width);
			return;
		}
		for(const MachineOperand& u : in.uses)
			if(u.kind == MachineOperand::Kind::FrameSlot) {
				U32 k = key(u.slot);
				if(in.isCall) {
					seen[k] = true;
					touches[block] = true;
					readWidth[k] = std::max(readWidth[k], (U32)u.width);
				} else {
					untracked[k] = true;
				}
			}
		for(const MachineOperand& d : in.defs)
			if(d.kind == MachineOperand::Kind::FrameSlot)
				untracked[key(d.slot)] = true;
	}

	void X86PeepholePass::TrackedSlots::finish() {
		for(U32 k = 0; k < keys; ++k)
			if(seen[k] && !untracked[k])
				index[k] = count++;
	}

	U32 X86PeepholePass::elimDeadSlotStores(MachineFunc& mf) {
		if(!slots.count)
			return 0;

		U32 nb = (U32)mf.blocks.size();
		U32 words = (slots.count + 63) / 64;
		List<U64> liveIn((U64)nb * words, 0); // block -> live slot bits at entry
		List<U64> cur(words, 0);

		List<U32> work;
		List<B32> queued;
		seedWork(mf, work, queued);
		while(!work.empty()) {
			U32 bi = work.back();
			work.pop_back();
			queued[bi] = false;
			const MachineBlock& b = mf.blocks[bi];
			slotBlockOut(b, liveIn, cur);
			for(U32 i = slots.touches[bi] ? (U32)b.insts.size() : 0; i-- > 0;)
				slotStep(b.insts[i], slots, cur);
			if(orInto(&liveIn[(U64)bi * words], cur))
				queuePreds(b, work, queued);
		}

		U32 removed = 0;
		for(MachineBlock& b : mf.blocks) {
			if(b.id < 0 || !slots.touches[(U32)b.id])
				continue;
			slotBlockOut(b, liveIn, cur);
			drop.assign(b.insts.size(), false);
			U32 here = 0;
			for(U32 i = (U32)b.insts.size(); i-- > 0;) {
				const MachineInstr& in = b.insts[i];
				if(isAnySlotStore(in)) {
					U32 bit = slots.index[slots.key(in.uses[0].slot)];
					if(bit != kNoBit && !((cur[bit >> 6] >> (bit & 63)) & 1)) {
						drop[i] = true;
						++here;
						continue; // dead
					}
				}
				slotStep(in, slots, cur);
			}
			removed += here;
			if(here)
				eraseMarked(b, drop);
		}
		return removed;
	}

	B32 X86PeepholePass::run(Module&, const Function&, MachineFunc& mf, const TargetInfo&) {
		seedQueued.clear();
		U32 changed = elimRedundantExt(mf);
		st.begin(mf.frameBytes / 8 + 1);
		slots.begin(mf);
		for(MachineBlock& b : mf.blocks)
			if(b.id >= 0)
				changed += runOnBlock(b);
		slots.finish();
		changed += elimDeadSlotStores(mf);
		return changed != 0;
	}
} // namespace rat
