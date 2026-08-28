#include "pass/emit/x86/x86_peephole.h"

#include "codegen/machine_function.h"
#include "codegen/machine_module.h"
#include "ir/module.h"

namespace rat {
	void X86PeepholePass::ValueState::reset() {
		killAllRegs();
		slot.clear();
		slotWidth.clear();
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
		auto it = slot.find(s);
		if(it == slot.end())
			return 0;
		auto wt = slotWidth.find(s);
		return wt != slotWidth.end() && wt->second == width ? it->second : 0;
	}

	void X86PeepholePass::ValueState::setSlot(I32 s, U32 width, U32 v) {
		slot[s] = v;
		slotWidth[s] = width;
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
		return (X86Op)in.op == X86Op::Store && in.regClass == detail::kGp && in.uses.size() == 2 &&
					 in.uses[0].kind == MachineOperand::Kind::FrameSlot && in.uses[1].isPhys();
	}

	// makeReload: defs[0] = register, uses[0] = frame slot
	B32 X86PeepholePass::isSlotLoad(const MachineInstr& in) {
		return (X86Op)in.op == X86Op::Load && in.regClass == detail::kGp && in.defs.size() == 1 &&
					 in.defs[0].isPhys() && in.uses.size() == 1 &&
					 in.uses[0].kind == MachineOperand::Kind::FrameSlot;
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

	U32 X86PeepholePass::runOnBlock(MachineBlock& b) {
		U32 changed = 0;
		List<MachineInstr> out;
		out.reserve(b.insts.size());
		st.reset();

		for(MachineInstr& in : b.insts) {
			X86Op op = (X86Op)in.op;

			if(isRegCopy(in)) {
				PhysReg d = in.defs[0].phys;
				PhysReg s = in.uses[0].phys;
				// both classes copy the full register, so the value carries exactly
				if(d == s || st.valueOf(d) == st.valueOf(s)) {
					++changed;
					continue;
				}
				st.setReg(d, st.valueOf(s));
				out.push_back(std::move(in));
				continue;
			}

			if(isSlotStore(in)) {
				I32 sl = in.uses[0].slot;
				U32 w = in.uses[1].width;
				U32 v = st.valueOf(in.uses[1].phys);
				if(st.slotValue(sl, w) == v) { // memory already has it
					++changed;
					continue;
				}
				st.setSlot(sl, w, v);
				out.push_back(std::move(in));
				continue;
			}

			if(isSlotLoad(in)) {
				I32 sl = in.uses[0].slot;
				U32 w = in.defs[0].width;
				PhysReg d = in.defs[0].phys;
				U32 v = st.slotValue(sl, w);
				if(v != 0) {
					if(st.valueOf(d) == v) { // the register already has it
						++changed;
						continue;
					}
					if(PhysReg src = st.regHolding(v, d); src != kNoReg) {
						out.push_back(makeCopy(d, src, in.regClass, w));
						st.setReg(d, v);
						++changed;
						continue;
					}
					st.setReg(d, v);
				} else {
					v = st.fresh();
					st.setReg(d, v);
					st.setSlot(sl, w, v);
				}
				out.push_back(std::move(in));
				continue;
			}

			if(!isTransparent(op) || writesUntrackedSlot(in)) {
				out.push_back(std::move(in));
				st.reset();
				continue;
			}
			if(op == X86Op::Call)
				st.killAllRegs(); // hidden scratch use in the argument shuffle
			for(const MachineOperand& d : in.defs)
				if(d.isPhys())
					st.killReg(d.phys);
			for(PhysReg p : in.clobbers)
				st.killReg(p);
			out.push_back(std::move(in));
		}

		b.insts = std::move(out);
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
			if(in.uses[i].isPhys() && in.uses[i].phys < kMaxPhys)
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

	// walk one instruction backwards, dem holds the demand after it on entry
	void X86PeepholePass::transfer(const MachineInstr& in, U64* dem) {
		X86Op op = (X86Op)in.op;
		U64 outD = 0;
		if(!in.defs.empty() && in.defs[0].isPhys() && in.defs[0].phys < kMaxPhys)
			outD = dem[in.defs[0].phys];
		for(const MachineOperand& d : in.defs)
			if(d.isPhys() && d.phys < kMaxPhys)
				dem[d.phys] = 0;
		for(PhysReg p : in.clobbers)
			if(p < kMaxPhys)
				dem[p] = 0;

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
				demandUses(in, kAllBits, dem, 0, 1);									// address
				demandUses(in, kAllBits, dem, 2);											// index
				if(in.uses[1].isPhys() && in.uses[1].phys < kMaxPhys) // stored value
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
		List<List<U64>> demIn(nb, List<U64>(kMaxPhys, 0));
		List<U64> cur(kMaxPhys, 0);

		B32 running = true;
		while(running) {
			running = false;
			for(U32 bi = nb; bi-- > 0;) {
				const MachineBlock& b = mf.blocks[bi];
				if(b.id < 0)
					continue;
				// no successors and no return
				B32 open = b.succs.empty() && !b.insts.empty() && (X86Op)b.insts.back().op != X86Op::Ret &&
									 (X86Op)b.insts.back().op != X86Op::Ud2;
				for(U32 i = 0; i < kMaxPhys; ++i)
					cur[i] = open ? kAllBits : 0;
				for(I32 s : b.succs)
					if(s >= 0 && s < (I32)nb)
						for(U32 i = 0; i < kMaxPhys; ++i)
							cur[i] |= demIn[(U32)s][i];
				for(U32 i = (U32)b.insts.size(); i-- > 0;)
					transfer(b.insts[i], cur.data());
				for(U32 i = 0; i < kMaxPhys; ++i)
					if((demIn[(U32)b.id][i] | cur[i]) != demIn[(U32)b.id][i]) {
						demIn[(U32)b.id][i] |= cur[i];
						running = true;
					}
			}
		}

		U32 removed = 0;
		for(MachineBlock& b : mf.blocks) {
			if(b.id < 0 || b.insts.empty())
				continue;
			for(U32 i = 0; i < kMaxPhys; ++i)
				cur[i] = 0;
			for(I32 s : b.succs)
				if(s >= 0 && s < (I32)nb)
					for(U32 i = 0; i < kMaxPhys; ++i)
						cur[i] |= demIn[(U32)s][i];
			if(b.succs.empty() && (X86Op)b.insts.back().op != X86Op::Ret &&
				 (X86Op)b.insts.back().op != X86Op::Ud2)
				for(U32 i = 0; i < kMaxPhys; ++i)
					cur[i] = kAllBits;

			List<B32> drop(b.insts.size(), false);
			U32 here = 0;
			for(U32 i = (U32)b.insts.size(); i-- > 0;) {
				const MachineInstr& in = b.insts[i];
				X86Op op = (X86Op)in.op;
				U32 n = (U32)in.imm;
				if(isNormalize(op) && n > 0 && n < 64 && !in.defs.empty() && in.defs[0].isPhys() &&
					 in.defs[0].phys < kMaxPhys && !(cur[in.defs[0].phys] & ~lowMask(n)) &&
					 flagSafeToDrop(b, i)) {
					drop[i] = true;
					++here;
					continue; // dead
				}
				transfer(in, cur.data());
			}
			if(!here)
				continue;
			removed += here;
			List<MachineInstr> out;
			out.reserve(b.insts.size());
			for(U32 i = 0; i < (U32)b.insts.size(); ++i)
				if(!drop[i])
					out.push_back(std::move(b.insts[i]));
			b.insts = std::move(out);
		}
		return removed;
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
	void X86PeepholePass::slotBlockOut(const MachineBlock& b,
																		 const List<List<U64>>& liveIn,
																		 List<U64>& cur) {
		B32 open = b.succs.empty() && !b.insts.empty() && (X86Op)b.insts.back().op != X86Op::Ret &&
							 (X86Op)b.insts.back().op != X86Op::Ud2;
		for(U32 i = 0; i < (U32)cur.size(); ++i)
			cur[i] = open ? kAllBits : 0;
		for(I32 s : b.succs)
			if(s >= 0 && s < (I32)liveIn.size())
				for(U32 i = 0; i < (U32)cur.size(); ++i)
					cur[i] |= liveIn[(U32)s][i];
	}

	// walk one instruction backwards over the tracked-slot liveness bits
	void X86PeepholePass::slotStep(const MachineInstr& in,
																 const Map<I32, U32>& slotIdx,
																 const Map<I32, U32>& readWidth,
																 List<U64>& cur) {
		if(isAnySlotStore(in)) {
			auto it = slotIdx.find(in.uses[0].slot);
			auto rw = readWidth.find(in.uses[0].slot);
			U32 widest = rw == readWidth.end() ? 0 : rw->second;
			if(it != slotIdx.end() && in.uses[1].width >= widest)
				cur[it->second >> 6] &= ~((U64)1 << (it->second & 63)); // full overwrite
			return;
		}
		if(isAnySlotLoad(in)) {
			auto it = slotIdx.find(in.uses[0].slot);
			if(it != slotIdx.end())
				cur[it->second >> 6] |= (U64)1 << (it->second & 63);
			return;
		}
		if(in.isCall)
			for(const MachineOperand& u : in.uses)
				if(u.kind == MachineOperand::Kind::FrameSlot)
					if(auto it = slotIdx.find(u.slot); it != slotIdx.end())
						cur[it->second >> 6] |= (U64)1 << (it->second & 63);
	}

	U32 X86PeepholePass::elimDeadSlotStores(MachineFunc& mf) {
		// a slot is tracked while it is only touched through the spill store and
		// reload shapes plus call stack arguments
		Set<I32> seen;
		Set<I32> untracked;
		Map<I32, U32> readWidth;
		for(const MachineBlock& b : mf.blocks) {
			if(b.id < 0)
				continue;
			for(const MachineInstr& in : b.insts) {
				if(isAnySlotStore(in)) {
					seen.insert(in.uses[0].slot);
					continue;
				}
				if(isAnySlotLoad(in)) {
					I32 s = in.uses[0].slot;
					seen.insert(s);
					readWidth[s] = std::max(readWidth[s], in.defs[0].width);
					continue;
				}
				for(const MachineOperand& u : in.uses)
					if(u.kind == MachineOperand::Kind::FrameSlot) {
						if(in.isCall) {
							seen.insert(u.slot);
							readWidth[u.slot] = std::max(readWidth[u.slot], u.width);
						} else {
							untracked.insert(u.slot);
						}
					}
				for(const MachineOperand& d : in.defs)
					if(d.kind == MachineOperand::Kind::FrameSlot)
						untracked.insert(d.slot);
			}
		}
		Map<I32, U32> slotIdx;
		for(I32 s : seen)
			if(!untracked.count(s))
				slotIdx.emplace(s, (U32)slotIdx.size());
		if(slotIdx.empty())
			return 0;

		U32 nb = (U32)mf.blocks.size();
		U32 words = ((U32)slotIdx.size() + 63) / 64;
		List<List<U64>> liveIn(nb, List<U64>(words, 0));
		List<U64> cur(words, 0);

		B32 running = true;
		while(running) {
			running = false;
			for(U32 bi = nb; bi-- > 0;) {
				const MachineBlock& b = mf.blocks[bi];
				if(b.id < 0)
					continue;
				slotBlockOut(b, liveIn, cur);
				for(U32 i = (U32)b.insts.size(); i-- > 0;)
					slotStep(b.insts[i], slotIdx, readWidth, cur);
				for(U32 i = 0; i < words; ++i)
					if((liveIn[(U32)b.id][i] | cur[i]) != liveIn[(U32)b.id][i]) {
						liveIn[(U32)b.id][i] |= cur[i];
						running = true;
					}
			}
		}

		U32 removed = 0;
		for(MachineBlock& b : mf.blocks) {
			if(b.id < 0 || b.insts.empty())
				continue;
			slotBlockOut(b, liveIn, cur);
			List<B32> drop(b.insts.size(), false);
			U32 here = 0;
			for(U32 i = (U32)b.insts.size(); i-- > 0;) {
				const MachineInstr& in = b.insts[i];
				if(isAnySlotStore(in)) {
					auto it = slotIdx.find(in.uses[0].slot);
					if(it != slotIdx.end() && !((cur[it->second >> 6] >> (it->second & 63)) & 1)) {
						drop[i] = true;
						++here;
						continue; // dead
					}
				}
				slotStep(in, slotIdx, readWidth, cur);
			}
			if(!here)
				continue;
			removed += here;
			List<MachineInstr> out;
			out.reserve(b.insts.size());
			for(U32 i = 0; i < (U32)b.insts.size(); ++i)
				if(!drop[i])
					out.push_back(std::move(b.insts[i]));
			b.insts = std::move(out);
		}
		return removed;
	}

	B32 X86PeepholePass::run(Module& module, MachineModule& mm, const TargetInfo&) {
		U32 changed = 0;
		for(const Function* fn : module) {
			MachineFunc& mf = mm.get(fn);
			for(U32 round = 0; round < 3; ++round)
				if(!elimRedundantExt(mf))
					break;
			for(MachineBlock& b : mf.blocks)
				if(b.id >= 0)
					changed += runOnBlock(b);
			changed += elimDeadSlotStores(mf);
		}
		return changed != 0;
	}
} // namespace rat
