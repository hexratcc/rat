#include "codegen/reg_alloc.h"

#include <cmath>

namespace rat {
	namespace detail {
		constexpr U32 kLoopUseWeight = 3;
		constexpr U32 kMaxUseWeight = 100000;
		// a bundle stops growing past this many segments, so merging stays cheap
		constexpr U32 kMaxBundleSegs = 256;

		PhysReg firstFree(const List<PhysReg>& regs, U64 blocked) {
			for(PhysReg p : regs)
				if(!((blocked >> p) & 1))
					return p;
			return kNoReg;
		}

		U32 loopWeight(I32 depth) {
			U32 weight = 1;
			for(I32 d = depth; d > 0 && weight < kMaxUseWeight; --d)
				weight *= kLoopUseWeight;
			return weight;
		}
	} // namespace detail

	B32 RegAllocPass::run(Module&, const Function&, MachineFunc& mf, const TargetInfo& target) {
		if(ri != target.registers())
			setup(target);
		fn = &mf;
		nv = mf.nextVReg;
		usedCallee = 0;
		number();
		liveness();
		List<Copy> copies;
		buildIntervals(copies);
		if(mf.dbg.size() > 1)
			checkDebugLive();
		coalesce(copies);
		assignRegs();
		rewriteDebug();
		rewrite();
		fn->usedCalleeSaved.clear();
		for(U64 m = usedCallee; m; m &= m - 1)
			fn->usedCalleeSaved.push_back((PhysReg)countTrailingZeros64(m));
		return true;
	}

	const RegAllocPass::Interval& RegAllocPass::bundle(VReg v) const { return iv[iv[v].root]; }

	U64 RegAllocPass::busyAt(U32 i) const { return busy[2 * (U64)i] | busy[2 * (U64)i + 1]; }

	void RegAllocPass::number() {
		blockFirst.assign(1, 0);
		for(const MachineBlock& blk : fn->blocks)
			blockFirst.push_back(blockFirst.back() + (U32)blk.insts.size());
		busy.assign(2 * (U64)blockFirst.back(), 0);
	}

	// a fixed register is busy from its def to its last use in the block (call argument
	// windows, div/shift operands, incoming arguments up to their copy), plus clobbers
	void RegAllocPass::pinFixed(const MachineInstr& in, U64 u, B32 copy, U64& live) {
		U64 defs = 0;
		U64 uses = 0;
		for(const MachineOperand& o : in.defs)
			if(o.isPhys())
				defs |= (U64)1 << o.phys;
		for(const MachineOperand& o : in.uses)
			if(o.isPhys())
				uses |= (U64)1 << o.phys;
		assert((in.isCall || !(in.clobbers & live)) && "clobber inside a fixed-register window");
		busy[u + 1] |= live | defs | in.clobbers;
		live &= ~(defs | in.clobbers);
		busy[u] |= live | uses | in.clobbers;
		if(!copy)
			busy[u + 1] |= uses;
		live |= uses;
	}

	void RegAllocPass::liveness() {
		U32 nb = (U32)fn->blocks.size();
		List<U32> defStamp(nv, 0), ueStamp(nv, 0);
		List<Pair<VReg, U32>> defs, ues;
		for(U32 b = 0; b < nb; ++b)
			for(const MachineInstr& in : fn->blocks[b].insts) {
				for(const MachineOperand& o : in.uses)
					if(o.isVReg() && defStamp[o.vreg] != b + 1 && ueStamp[o.vreg] != b + 1) {
						ueStamp[o.vreg] = b + 1;
						ues.emplace_back(o.vreg, b);
					}
				for(const MachineOperand& o : in.defs)
					if(o.isVReg() && defStamp[o.vreg] != b + 1) {
						defStamp[o.vreg] = b + 1;
						defs.emplace_back(o.vreg, b);
					}
			}
		cross.assign(nv, 0);
		if(local) {
			for(const auto& [v, b] : ues)
				cross[v] = 1;
			outFirst.assign(nb + 1, 0);
			return;
		}
		List<U32> defFirst, defBlocks, ueFirst, ueBlocks;
		detail::groupBy(defs, nv, defFirst, defBlocks);
		detail::groupBy(ues, nv, ueFirst, ueBlocks);
		List<VReg> defIn(nb, kNoVReg), liveIn(nb, kNoVReg), outStamp(nb, kNoVReg);
		List<U32> work;
		List<Pair<U32, VReg>> outs;
		for(VReg v = 1; v < nv; ++v) {
			if(ueFirst[v] == ueFirst[v + 1])
				continue;
			for(U32 k = defFirst[v]; k < defFirst[v + 1]; ++k)
				defIn[defBlocks[k]] = v;
			for(U32 k = ueFirst[v]; k < ueFirst[v + 1]; ++k) {
				liveIn[ueBlocks[k]] = v;
				work.push_back(ueBlocks[k]);
			}
			while(!work.empty()) {
				U32 x = work.back();
				work.pop_back();
				for(I32 p : fn->blocks[x].preds) {
					if(outStamp[p] != v) {
						outStamp[p] = v;
						outs.emplace_back(p, v);
					}
					if(defIn[p] != v && liveIn[p] != v) {
						liveIn[p] = v;
						work.push_back(p);
					}
				}
			}
		}
		detail::groupBy(outs, nb, outFirst, outVRegs);
	}

	void RegAllocPass::Interval::reset(VReg self) {
		segs.clear();
		weight = 0;
		root = self;
		hint = kNoReg;
		reg = kNoReg;
		slot = 0;
	}

	// segments come in descending order per vreg, merge touching ones
	void RegAllocPass::addSeg(VReg v, I32 start, I32 end) {
		List<Seg>& segs = iv[v].segs;
		if(!segs.empty() && end + 1 >= segs.back().first)
			segs.back().first = std::min(segs.back().first, start);
		else
			segs.emplace_back(start, end);
	}

	void RegAllocPass::noteCopy(List<Copy>& copies, const MachineInstr& in, U32 weight) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		B32 sameClassVRegs = d.isVReg() && s.isVReg() && fn->vregClass[d.vreg] == fn->vregClass[s.vreg];
		if(sameClassVRegs && !cross[d.vreg] && !cross[s.vreg])
			copies.emplace_back(-(I32)weight, d.vreg, s.vreg);
		else if(d.isVReg() && s.isPhys())
			iv[d.vreg].hint = s.phys;
		else if(d.isPhys() && s.isVReg())
			iv[s.vreg].hint = d.phys;
	}

	// backward walk per block from its live-out set, blocks in reverse
	void RegAllocPass::buildIntervals(List<Copy>& copies) {
		if(iv.size() < nv)
			iv.resize(nv);
		for(VReg v = 0; v < nv; ++v)
			iv[v].reset(v);
		List<U8> live(nv, 0);
		List<I32> segEnd(nv, 0);
		List<VReg> liveList;
		for(U32 b = (U32)fn->blocks.size(); b-- > 0;) {
			const MachineBlock& blk = fn->blocks[b];
			if(blk.insts.empty())
				continue;
			for(U32 k = outFirst[b]; k < outFirst[b + 1]; ++k) {
				VReg v = outVRegs[k];
				live[v] = 1;
				segEnd[v] = 2 * (I32)blockFirst[b + 1] - 1;
				liveList.push_back(v);
			}
			U32 weight = detail::loopWeight(blk.loopDepth);
			U64 liveFixed = 0;
			for(U32 k = (U32)blk.insts.size(); k-- > 0;) {
				const MachineInstr& in = blk.insts[k];
				I32 u = 2 * (I32)(blockFirst[b] + k);
				B32 copy = isCopy(in);
				pinFixed(in, (U64)u, copy, liveFixed);
				for(const MachineOperand& o : in.defs) {
					if(!o.isVReg() || cross[o.vreg])
						continue;
					iv[o.vreg].weight += (F32)weight;
					I32 end = live[o.vreg] ? segEnd[o.vreg] : u + 1; // a dead def still takes its slot
					addSeg(o.vreg, u + 1, end);
					live[o.vreg] = 0;
				}
				I32 useEnd = u + 1;
				if(copy) {
					useEnd = u;
					noteCopy(copies, in, weight);
				}
				for(const MachineOperand& o : in.uses) {
					if(!o.isVReg() || cross[o.vreg])
						continue;
					iv[o.vreg].weight += (F32)weight;
					if(live[o.vreg])
						continue;
					live[o.vreg] = 1;
					segEnd[o.vreg] = useEnd;
					liveList.push_back(o.vreg);
				}
			}
			for(VReg v : liveList)
				if(live[v]) {
					addSeg(v, 2 * (I32)blockFirst[b], segEnd[v]);
					live[v] = 0;
				}
			liveList.clear();
		}
		for(VReg v = 0; v < nv; ++v)
			std::reverse(iv[v].segs.begin(), iv[v].segs.end());
		chunk.assign((busy.size() + 63) / 64, 0);
		for(U64 s = 0; s < busy.size(); ++s)
			chunk[s >> 6] |= busy[s];
	}

	B32 RegAllocPass::covers(VReg v, I32 pos) const {
		const List<Seg>& segs = iv[v].segs;
		auto it = std::upper_bound(segs.begin(), segs.end(), Seg{pos, INT32_MAX});
		return it != segs.begin() && (it - 1)->second >= pos;
	}

	// a record holds when its vreg is live right before the instr, or was just written by the one ahead,
	// before bundles merge. stack resident vregs keep their slot
	void RegAllocPass::checkDebugLive() {
		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			MachineBlock& blk = fn->blocks[b];
			for(U32 k = 0; k <= blk.insts.size(); ++k) {
				I32 u = 2 * (I32)(blockFirst[b] + k);
				for(U32 r = blk.dbgBefore(k); r; r = fn->dbg[r].next) {
					MachineOperand& o = fn->dbg[r].loc;
					if(!o.isVReg() || cross[o.vreg])
						continue;
					B32 liveBefore = k < blk.insts.size() && covers(o.vreg, u);
					B32 justWritten = k > 0 && covers(o.vreg, u - 1);
					if(!liveBefore && !justWritten)
						o = MachineOperand::none(o.width);
				}
			}
		}
	}

	void RegAllocPass::rewriteDebug() {
		for(DbgRec& r : fn->dbg) {
			if(!r.loc.isVReg())
				continue;
			const Interval& t = bundle(r.loc.vreg);
			if(t.reg != kNoReg)
				r.loc = MachineOperand::fixed(t.reg, r.loc.width);
			else
				r.loc = MachineOperand::frameSlot(t.slot, r.loc.width);
		}
	}

	// path halving
	VReg RegAllocPass::find(VReg v) {
		while(iv[v].root != v) {
			iv[v].root = iv[iv[v].root].root;
			v = iv[v].root;
		}
		return v;
	}

	B32 RegAllocPass::overlaps(VReg a, VReg b) const {
		const List<Seg>& x = iv[a].segs;
		const List<Seg>& y = iv[b].segs;
		U32 i = 0;
		U32 j = 0;
		while(i < x.size() && j < y.size()) {
			if(x[i].second < y[j].first)
				++i;
			else if(y[j].second < x[i].first)
				++j;
			else
				return true;
		}
		return false;
	}

	// b joins a, their segments do not overlap
	void RegAllocPass::merge(VReg a, VReg b, List<Seg>& merged) {
		Interval& t = iv[a];
		Interval& o = iv[b];
		merged.resize(t.segs.size() + o.segs.size());
		std::merge(t.segs.begin(), t.segs.end(), o.segs.begin(), o.segs.end(), merged.begin());
		t.segs.clear();
		for(const auto& [start, end] : merged) // join touching segments
			if(!t.segs.empty() && t.segs.back().second + 1 == start)
				t.segs.back().second = end;
			else
				t.segs.emplace_back(start, end);
		o.segs.clear();
		t.weight += o.weight;
		if(t.hint == kNoReg)
			t.hint = o.hint;
		o.root = a;
	}

	// copy-related vregs whose ranges do not overlap share one register or slot, hottest
	// copies first
	void RegAllocPass::coalesce(List<Copy>& copies) {
		std::sort(copies.begin(), copies.end());
		List<Seg> merged;
		for(const auto& [negWeight, def, src] : copies) {
			VReg a = find(def);
			VReg b = find(src);
			U64 bundleSegs = iv[a].segs.size() + iv[b].segs.size();
			if(a != b && bundleSegs <= detail::kMaxBundleSegs && !overlaps(a, b))
				merge(a, b, merged);
		}
		for(VReg v = 1; v < nv; ++v) {
			iv[v].root = find(v);
			I32 len = 0;
			for(const auto& [start, end] : iv[v].segs)
				len += end - start + 1;
			if(len)
				iv[v].weight /= std::sqrt((F32)len);
		}
	}

	PhysReg RegAllocPass::pick(VReg v) const {
		U32 cls = fn->vregClass[v];
		U64 blocked = ~allocMask[cls];
		for(const auto& [start, end] : iv[v].segs)
			for(I32 s = start; s <= end && blocked != ~0ull;) {
				if((s & 63) == 0 && s + 63 <= end) {
					blocked |= chunk[(U64)s >> 6];
					s += 64;
				} else {
					blocked |= busy[(U64)s++];
				}
			}
		PhysReg hint = iv[v].hint;
		if(hint != kNoReg && !((blocked >> hint) & 1))
			return hint;
		return detail::firstFree(ri->classes[cls].allocatable, blocked);
	}

	void RegAllocPass::assignRegs() {
		List<Pair<F32, VReg>> order; // (-weight, bundle), heaviest first
		for(VReg v = 1; v < nv; ++v)
			if(!iv[v].segs.empty())
				order.emplace_back(-iv[v].weight, v);
		if(!local) // few locals spill, their order hardly matters
			std::sort(order.begin(), order.end());
		List<Pair<I32, VReg>> spilled; // (start, bundle)
		for(const auto& [negWeight, v] : order) {
			Interval& t = iv[v];
			assert(!ri->classes[fn->vregClass[v]].scratch.empty() && "vreg of a class with no scratch");
			t.reg = pick(v);
			if(t.reg == kNoReg) {
				spilled.emplace_back(t.segs.front().first, v);
				continue;
			}
			U64 bit = (U64)1 << t.reg;
			usedCallee |= bit & calleeMask;
			for(const auto& [start, end] : t.segs) {
				for(I32 s = start; s <= end; ++s)
					busy[(U64)s] |= bit;
				for(I32 c = start >> 6; c <= end >> 6; ++c)
					chunk[(U64)c] |= bit;
			}
		}
		assignSlots(spilled);
		for(VReg v = 1; v < nv; ++v)
			if(cross[v]) {
				U32 cls = fn->vregClass[v];
				iv[v].slot = hooks.allocSlot(*fn, cls, ri->classes[cls].spillBytes);
			}
	}

	void RegAllocPass::assignSlots(List<Pair<I32, VReg>>& spilled) {
		std::sort(spilled.begin(), spilled.end());
		List<Pair<I32, I32>> pool[kMaxRegClasses]; // (slot, end of its last holder)
		for(const auto& [start, v] : spilled) {
			U32 cls = fn->vregClass[v];
			List<Pair<I32, I32>>& slots = pool[cls];
			U32 k = 0;
			while(k < slots.size() && slots[k].second >= start)
				++k;
			if(k == slots.size())
				slots.emplace_back(hooks.allocSlot(*fn, cls, ri->classes[cls].spillBytes), 0);
			iv[v].slot = slots[k].first;
			slots[k].second = iv[v].segs.back().second;
		}
	}

	PhysReg RegAllocPass::pickTemp(U32 cls, U64 hard, U64 soft) {
		const RegClass& rc = ri->classes[cls];
		PhysReg p = detail::firstFree(rc.scratch, hard | soft);
		if(p == kNoReg)
			p = detail::firstFree(rc.allocatable, hard | soft);
		if(p == kNoReg)
			p = detail::firstFree(rc.scratch, hard);
		assert(p != kNoReg && "no free register for a spilled operand");
		usedCallee |= ((U64)1 << p) & calleeMask;
		return p;
	}

	PhysReg RegAllocPass::spillReg(
			List<MachineInstr>& out, const MachineOperand& o, U32 i, U64 clobbers, B32 use) {
		VReg root = iv[o.vreg].root;
		U64 taken = 0;
		for(const auto& [v, r] : temps) {
			if(v == root)
				return r;
			taken |= (U64)1 << r;
		}
		U32 cls = fn->vregClass[o.vreg];
		U64 hard = busyAt(i) & ~clobbers;
		if(use)
			hard |= taken;
		PhysReg r = pickTemp(cls, hard, clobbers | taken);
		if(use)
			hooks.makeReload(out.emplace_back(), r, iv[root].slot, cls, o.width);
		temps.emplace_back(root, r);
		return r;
	}

	void RegAllocPass::rewriteInstr(List<MachineInstr>& out, MachineInstr& in, U32 i) {
		temps.clear();
		for(MachineOperand& o : in.uses) {
			if(!o.isVReg())
				continue;
			const Interval& t = bundle(o.vreg);
			if(t.reg != kNoReg)
				o = MachineOperand::fixed(t.reg, o.width);
			else if(in.isCall) // the call reads the slot itself
				o = MachineOperand::frameSlot(t.slot, o.width);
			else
				o = MachineOperand::fixed(spillReg(out, o, i, in.clobbers, true), o.width);
		}
		U32 at = (U32)out.size(); // spill stores go after the instr
		for(MachineOperand& o : in.defs) {
			if(!o.isVReg())
				continue;
			const Interval& t = bundle(o.vreg);
			PhysReg r = t.reg;
			if(r == kNoReg) {
				r = spillReg(out, o, i, in.clobbers, false);
				hooks.makeSpill(out.emplace_back(), t.slot, r, fn->vregClass[o.vreg], o.width);
			}
			o = MachineOperand::fixed(r, o.width);
		}
		out.insert(out.begin() + at, std::move(in));
	}

	B32 RegAllocPass::isCopy(const MachineInstr& in) const {
		return in.defs.size() == 1 && in.uses.size() == 1 && hooks.isCopy(in);
	}

	// a copy inside a bundle vanishes, one between a register and a spilled bundle is its reload or
	// its spill
	B32 RegAllocPass::rewriteCopy(List<MachineInstr>& out, const MachineInstr& in, U32 i) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		if(d.isVReg() && s.isVReg() && iv[d.vreg].root == iv[s.vreg].root)
			return true;
		if(!(d.isVReg() || d.isPhys()) || !(s.isVReg() || s.isPhys()))
			return false;
		PhysReg dr = d.isPhys() ? d.phys : bundle(d.vreg).reg;
		PhysReg sr = s.isPhys() ? s.phys : bundle(s.vreg).reg;
		if(dr != kNoReg && sr != kNoReg)
			return false;
		if(sr == kNoReg) {
			if(dr == kNoReg)
				dr = pickTemp(fn->vregClass[s.vreg], busyAt(i), 0);
			hooks.makeReload(out.emplace_back(), dr, bundle(s.vreg).slot, fn->vregClass[s.vreg], s.width);
			sr = dr;
		}
		if(d.isVReg() && bundle(d.vreg).reg == kNoReg)
			hooks.makeSpill(out.emplace_back(), bundle(d.vreg).slot, sr, fn->vregClass[d.vreg], d.width);
		return true;
	}

	// debug records move to the first instr standing for theirs
	void RegAllocPass::rewrite() {
		List<MachineInstr> out;
		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			MachineBlock& blk = fn->blocks[b];
			out.clear();
			out.reserve(blk.insts.size());
			U32 dbg = 0;
			for(U32 k = 0; k < blk.insts.size(); ++k) {
				MachineInstr& in = blk.insts[k];
				U32 i = blockFirst[b] + k;
				U32 first = (U32)out.size();
				dbg = fn->joinDbg(dbg, in.dbg);
				in.dbg = 0;
				if(!isCopy(in) || !rewriteCopy(out, in, i))
					rewriteInstr(out, in, i);
				if(out.size() > first) {
					out[first].dbg = dbg;
					dbg = 0;
				}
			}
			blk.dbgEnd = fn->joinDbg(dbg, blk.dbgEnd);
			blk.insts.swap(out);
		}
	}

	void RegAllocPass::setup(const TargetInfo& target) {
		ri = target.registers();
		hooks = target.regAllocHooks();
		for(U32 cls = 0; cls < ri->classes.size(); ++cls) {
			const RegClass& rc = ri->classes[cls];
			for(PhysReg p : rc.allocatable)
				allocMask[cls] |= (U64)1 << p;
			for(PhysReg p : rc.calleeSaved)
				calleeMask |= (U64)1 << p;
		}
	}
} // namespace rat
