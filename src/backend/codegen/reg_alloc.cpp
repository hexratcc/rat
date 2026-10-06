#include "codegen/reg_alloc.h"

#include <cmath>
#include <cstring>

#include "target/target.h"

namespace rat {
	namespace detail {
		// per-loop-level operand weight
		constexpr U32 kLoopUseWeight = 3;
		constexpr U32 kMaxUseWeight = 100000;
		// copy key
		constexpr U32 kMaxLevel = 15;
		constexpr U32 kCopyVRegMask = (1u << 30) - 1;
		// a bundle stops growing past this many segments, so merging stays cheap
		constexpr U32 kMaxBundleSegs = 256;

		PhysReg firstFree(const List<PhysReg>& regs, U64 blocked) {
			for(PhysReg p : regs)
				if(!((blocked >> p) & 1))
					return p;
			return kNoReg;
		}

		void groupByVReg(const List<Pair<VReg, U32>>& in, U32 nv, List<U32>& first, List<U32>& out) {
			first.assign(nv + 1, 0);
			for(const auto& [v, b] : in)
				++first[v + 1];
			for(U32 v = 0; v < nv; ++v)
				first[v + 1] += first[v];
			List<U32> pos(first.begin(), first.end() - 1);
			out.resize(in.size());
			for(const auto& [v, b] : in)
				out[pos[v]++] = b;
		}
	} // namespace detail

	void RegAllocPass::allocate(MachineFunc& f) {
		fn = &f;
		nv = f.nextVReg;
		assert(nv <= detail::kCopyVRegMask && "too many vregs for a copy key");
		usedCallee = 0;
		copies.clear();
		iv.clear();
		number();
		liveness();
		buildIntervals();
		coalesce();
		assignRegs();
		rewrite();
		fn->usedCalleeSaved.clear();
		for(U64 m = usedCallee; m; m &= m - 1)
			fn->usedCalleeSaved.push_back((PhysReg)countTrailingZeros64(m));
	}

	B32 RegAllocPass::isCopy(const MachineInstr& in) const {
		return hooks.isCopy && in.defs.size() == 1 && in.uses.size() == 1 && hooks.isCopy(in);
	}

	const RegAllocPass::Interval& RegAllocPass::bundle(VReg v) const { return iv[iv[v].root]; }

	B32 RegAllocPass::sameBundle(const MachineOperand& a, const MachineOperand& b) const {
		return a.isVReg() && b.isVReg() && iv[a.vreg].root == iv[b.vreg].root;
	}

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
		U64 clob = in.clobbers;
		assert((in.isCall || !(clob & live)) && "clobber inside a fixed-register window");
		busy[u + 1] |= live | defs | clob;
		live &= ~(defs | clob);
		busy[u] |= live | uses | clob;
		if(!copy)
			busy[u + 1] |= uses;
		live |= uses;
	}

	void RegAllocPass::liveness() {
		U32 nb = (U32)fn->blocks.size();
		List<U32> defStamp(nv, 0);
		List<U32> ueStamp(nv, 0);
		List<Pair<VReg, U32>> defs; // (vreg, block), blocks ascending
		List<Pair<VReg, U32>> ues;
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
		List<U32> defFirst;
		List<U32> defBlocks;
		List<U32> ueFirst;
		List<U32> ueBlocks;
		detail::groupByVReg(defs, nv, defFirst, defBlocks);
		detail::groupByVReg(ues, nv, ueFirst, ueBlocks);
		List<VReg> defIn(nb, kNoVReg);
		List<VReg> liveIn(nb, kNoVReg);
		List<VReg> outStamp(nb, kNoVReg);
		List<U32> work;
		liveOut.assign(nb, {});
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
						liveOut[p].push_back(v);
					}
					if(defIn[p] != v && liveIn[p] != v) {
						liveIn[p] = v;
						work.push_back(p);
					}
				}
			}
		}
	}

	// segments come in descending order per vreg, merge touching ones
	void RegAllocPass::addSeg(VReg v, I32 start, I32 end) {
		List<Seg>& segs = iv[v].segs;
		if(!segs.empty() && end + 1 >= segs.back().first)
			segs.back().first = std::min(segs.back().first, start);
		else
			segs.emplace_back(start, end);
	}

	void RegAllocPass::noteCopy(const MachineInstr& in, U32 level) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		if(d.isVReg() && s.isVReg() && fn->vregClass[d.vreg] == fn->vregClass[s.vreg])
			copies.push_back((U64)(detail::kMaxLevel - level) << 60 | (U64)d.vreg << 30 | s.vreg);
		else if(d.isVReg() && s.isPhys())
			iv[d.vreg].hint = s.phys;
		else if(d.isPhys() && s.isVReg())
			iv[s.vreg].hint = d.phys;
	}

	// backward walk per block from its live-out set, blocks in reverse
	void RegAllocPass::buildIntervals() {
		iv.resize(nv);
		for(VReg v = 0; v < nv; ++v)
			iv[v].root = v;
		List<U8> live(nv, 0);
		List<I32> segEnd(nv, 0);
		List<VReg> liveList;
		for(U32 b = (U32)fn->blocks.size(); b-- > 0;) {
			const MachineBlock& blk = fn->blocks[b];
			if(blk.insts.empty())
				continue;
			for(VReg v : liveOut[b]) {
				live[v] = 1;
				segEnd[v] = 2 * (I32)blockFirst[b + 1] - 1;
				liveList.push_back(v);
			}
			U32 weight = 1;
			U32 level = 0; // loop levels counted in weight
			for(I32 d = blk.loopDepth; d > 0 && weight < detail::kMaxUseWeight; --d) {
				weight *= detail::kLoopUseWeight;
				++level;
			}
			U64 fixed = 0; // live fixed registers
			for(U32 k = (U32)blk.insts.size(); k-- > 0;) {
				const MachineInstr& in = blk.insts[k];
				I32 u = 2 * (I32)(blockFirst[b] + k);
				B32 copy = isCopy(in);
				pinFixed(in, (U64)u, copy, fixed);
				for(const MachineOperand& o : in.defs) {
					if(!o.isVReg())
						continue;
					iv[o.vreg].weight += (F32)weight;
					I32 end = u + 1; // a dead def still takes its slot
					if(live[o.vreg])
						end = segEnd[o.vreg];
					addSeg(o.vreg, u + 1, end);
					live[o.vreg] = 0;
				}
				I32 useEnd = u + 1;
				if(copy) {
					useEnd = u;
					noteCopy(in, level);
				}
				for(const MachineOperand& o : in.uses) {
					if(!o.isVReg())
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
		for(Interval& t : iv)
			std::reverse(t.segs.begin(), t.segs.end());
		chunk.assign((busy.size() + 63) / 64, 0);
		for(U64 s = 0; s < busy.size(); ++s)
			chunk[s >> 6] |= busy[s];
	}

	// path halving
	VReg RegAllocPass::find(VReg v) {
		while(iv[v].root != v)
			v = iv[v].root = iv[iv[v].root].root;
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
	void RegAllocPass::merge(VReg a, VReg b) {
		Interval& t = iv[a];
		Interval& o = iv[b];
		List<Seg> segs(t.segs.size() + o.segs.size());
		std::merge(t.segs.begin(), t.segs.end(), o.segs.begin(), o.segs.end(), segs.begin());
		t.segs.clear();
		for(const auto& [start, end] : segs) // join touching segments
			if(!t.segs.empty() && t.segs.back().second + 1 == start)
				t.segs.back().second = end;
			else
				t.segs.emplace_back(start, end);
		o.segs = {};
		t.weight += o.weight;
		if(t.hint == kNoReg)
			t.hint = o.hint;
		o.root = a;
	}

	// copy-related vregs whose ranges do not overlap share one register or slot, hottest
	// copies first
	void RegAllocPass::coalesce() {
		std::sort(copies.begin(), copies.end());
		for(U64 c : copies) {
			VReg a = find((VReg)(c >> 30) & detail::kCopyVRegMask);
			VReg b = find((VReg)c & detail::kCopyVRegMask);
			if(a != b && iv[a].segs.size() + iv[b].segs.size() <= detail::kMaxBundleSegs &&
				 !overlaps(a, b))
				merge(a, b);
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
		const RegClass& rc = ri->classes[fn->vregClass[v]];
		U64 blocked = ~allocMask[rc.id];
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
		return detail::firstFree(rc.allocatable, blocked);
	}

	void RegAllocPass::assignRegs() {
		List<U64> order; // (inverted weight bits, bundle), weights are positive
		for(VReg v = 1; v < nv; ++v) {
			if(iv[v].segs.empty())
				continue;
			U32 bits = 0;
			std::memcpy(&bits, &iv[v].weight, sizeof(bits));
			order.push_back((U64)~bits << 32 | v);
		}
		std::sort(order.begin(), order.end());
		List<Pair<I32, VReg>> spilled; // (start, bundle)
		for(U64 key : order) {
			VReg v = (VReg)key;
			Interval& t = iv[v];
			assert(!ri->classes[fn->vregClass[v]].scratch.empty() && "vreg of a class with no scratch");
			t.reg = pick(v);
			if(t.reg == kNoReg) {
				spilled.emplace_back(t.segs.front().first, v);
				continue;
			}
			usedCallee |= ((U64)1 << t.reg) & calleeMask;
			for(const auto& [start, end] : t.segs) {
				for(I32 s = start; s <= end; ++s)
					busy[(U64)s] |= (U64)1 << t.reg;
				for(I32 c = start >> 6; c <= end >> 6; ++c)
					chunk[(U64)c] |= (U64)1 << t.reg;
			}
		}
		assignSlots(spilled);
	}

	void RegAllocPass::assignSlots(List<Pair<I32, VReg>>& spilled) {
		std::sort(spilled.begin(), spilled.end());
		List<Pair<I32, I32>> pool[kMaxRegClasses]; // (slot, end of its last holder)
		for(const auto& [start, v] : spilled) {
			Interval& t = iv[v];
			U32 cls = fn->vregClass[v];
			I32 end = t.segs.back().second;
			B32 reused = false;
			for(auto& [slot, freeAt] : pool[cls])
				if(freeAt < start) {
					t.slot = slot;
					freeAt = end;
					reused = true;
					break;
				}
			if(reused)
				continue;
			U32 bytes = ri->classes[cls].spillBytes;
			if(!bytes)
				bytes = ri->spillSlotBytes;
			t.slot = hooks.allocSlot(*fn, cls, bytes);
			pool[cls].emplace_back(t.slot, end);
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

	PhysReg RegAllocPass::spillReg(List<MachineInstr>& out, const MachineOperand& o, U32 i, B32 use) {
		VReg root = iv[o.vreg].root;
		for(const auto& [v, r] : temps)
			if(v == root)
				return r;
		U32 cls = fn->vregClass[o.vreg];
		U64 hard = (busy[2 * (U64)i] | busy[2 * (U64)i + 1]) & ~own;
		if(use)
			hard |= taken;
		PhysReg r = pickTemp(cls, hard, own | taken);
		if(use)
			hooks.makeReload(out.emplace_back(), r, iv[root].slot, cls, o.width);
		taken |= (U64)1 << r;
		temps.emplace_back(root, r);
		return r;
	}

	void RegAllocPass::rewriteInstr(List<MachineInstr>& out, MachineInstr& in, U32 i) {
		temps.clear();
		taken = 0;
		own = in.clobbers;
		stores.clear();
		for(MachineOperand& o : in.uses) {
			if(!o.isVReg())
				continue;
			PhysReg r = bundle(o.vreg).reg;
			if(r == kNoReg && in.isCall) { // the call reads the slot itself
				o = MachineOperand::frameSlot(bundle(o.vreg).slot, o.width);
				continue;
			}
			if(r == kNoReg)
				r = spillReg(out, o, i, true);
			o = MachineOperand::fixed(r, o.width);
		}
		for(MachineOperand& o : in.defs) {
			if(!o.isVReg())
				continue;
			const Interval& t = bundle(o.vreg);
			PhysReg r = t.reg;
			if(r == kNoReg) {
				r = spillReg(out, o, i, false);
				hooks.makeSpill(stores.emplace_back(), t.slot, r, fn->vregClass[o.vreg], o.width);
			}
			o = MachineOperand::fixed(r, o.width);
		}
		out.push_back(std::move(in));
		for(MachineInstr& s : stores)
			out.push_back(std::move(s));
	}

	// a copy between a register and a spilled bundle is its reload or its spill
	B32 RegAllocPass::rewriteCopy(List<MachineInstr>& out, const MachineInstr& in, U32 i) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		if(!(d.isVReg() || d.isPhys()) || !(s.isVReg() || s.isPhys()))
			return false;
		PhysReg dr = regOf(d);
		PhysReg sr = regOf(s);
		if(dr != kNoReg && sr != kNoReg)
			return false;
		if(sr == kNoReg) {
			if(dr == kNoReg)
				dr = pickTemp(fn->vregClass[s.vreg], busy[2 * (U64)i] | busy[2 * (U64)i + 1], 0);
			hooks.makeReload(out.emplace_back(), dr, bundle(s.vreg).slot, fn->vregClass[s.vreg], s.width);
			sr = dr;
		}
		if(d.isVReg() && bundle(d.vreg).reg == kNoReg)
			hooks.makeSpill(out.emplace_back(), bundle(d.vreg).slot, sr, fn->vregClass[d.vreg], d.width);
		return true;
	}

	PhysReg RegAllocPass::regOf(const MachineOperand& o) const {
		if(o.isPhys())
			return o.phys;
		return bundle(o.vreg).reg;
	}

	// copies inside a bundle vanish
	void RegAllocPass::rewrite() {
		List<MachineInstr> out;
		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			List<MachineInstr>& insts = fn->blocks[b].insts;
			out.clear();
			out.reserve(insts.size());
			for(U32 k = 0; k < insts.size(); ++k) {
				MachineInstr& in = insts[k];
				U32 i = blockFirst[b] + k;
				if(isCopy(in) && (sameBundle(in.defs[0], in.uses[0]) || rewriteCopy(out, in, i)))
					continue;
				rewriteInstr(out, in, i);
			}
			insts.swap(out);
		}
	}

	B32 RegAllocPass::run(Module&, const Function&, MachineFunc& mf, const TargetInfo& target) {
		if(ri != target.registers())
			setup(target);
		allocate(mf);
		return true;
	}

	void RegAllocPass::setup(const TargetInfo& target) {
		ri = target.registers();
		hooks = target.regAllocHooks();
		for(const RegClass& rc : ri->classes) {
			for(PhysReg p : rc.allocatable)
				allocMask[rc.id] |= (U64)1 << p;
			for(PhysReg p : rc.calleeSaved)
				calleeMask |= (U64)1 << p;
		}
	}
} // namespace rat
