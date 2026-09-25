#include "codegen/reg_alloc.h"

#include <cmath>

#include "codegen/machine_module.h"
#include "ir/module.h"
#include "target/target.h"

namespace rat {
	namespace detail {
		// per-loop-level operand weight
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
	} // namespace detail

	detail::RegAllocFunc::RegAllocFunc(MachineFunc& f, const RegisterInfo& r, const RegAllocHooks& h)
	: fn(f),
		ri(r),
		hooks(h),
		nv(f.nextVReg) {}

	void detail::RegAllocFunc::run() {
		for(const RegClass& rc : ri.classes) {
			for(PhysReg p : rc.allocatable)
				allocMask[rc.id] |= (U64)1 << p;
			for(PhysReg p : rc.calleeSaved)
				calleeMask |= (U64)1 << p;
		}
		number();
		liveness();
		buildIntervals();
		coalesce();
		assignRegs();
		rewrite();
		fn.usedCalleeSaved.clear();
		for(U64 m = usedCallee; m; m &= m - 1)
			fn.usedCalleeSaved.push_back((PhysReg)countTrailingZeros64(m));
	}

	B32 detail::RegAllocFunc::isCopy(const MachineInstr& in) const {
		return hooks.isCopy && in.defs.size() == 1 && in.uses.size() == 1 && hooks.isCopy(in);
	}

	const detail::RaInterval& detail::RegAllocFunc::bundle(VReg v) const { return iv[iv[v].root]; }

	B32 detail::RegAllocFunc::sameBundle(const MachineOperand& a, const MachineOperand& b) const {
		return a.isVReg() && b.isVReg() && iv[a.vreg].root == iv[b.vreg].root;
	}

	void detail::RegAllocFunc::number() {
		blockFirst.assign(1, 0);
		for(const MachineBlock& blk : fn.blocks)
			blockFirst.push_back(blockFirst.back() + (U32)blk.insts.size());
		busy.assign(2 * (U64)blockFirst.back(), 0);
		for(U32 b = 0; b < fn.blocks.size(); ++b)
			pinFixed(b);
	}

	// a fixed register is busy from its def to its last use in the block (call argument
	// windows, div/shift operands, incoming arguments up to their copy), plus clobbers
	void detail::RegAllocFunc::pinFixed(U32 b) {
		const List<MachineInstr>& insts = fn.blocks[b].insts;
		U64 live = 0;
		for(U32 k = (U32)insts.size(); k-- > 0;) {
			const MachineInstr& in = insts[k];
			U64 u = 2 * (U64)(blockFirst[b] + k);
			U64 defs = 0;
			U64 uses = 0;
			U64 clob = 0;
			for(const MachineOperand& o : in.defs)
				if(o.isPhys())
					defs |= (U64)1 << o.phys;
			for(const MachineOperand& o : in.uses)
				if(o.isPhys())
					uses |= (U64)1 << o.phys;
			for(PhysReg p : in.clobbers)
				clob |= (U64)1 << p;
			assert((in.isCall || !(clob & live)) && "clobber inside a fixed-register window");
			busy[u + 1] |= live | defs | clob;
			live &= ~(defs | clob);
			busy[u] |= live | uses | clob;
			if(!isCopy(in))
				busy[u + 1] |= uses;
			live |= uses;
		}
	}

	void detail::RegAllocFunc::liveness() {
		U32 nb = (U32)fn.blocks.size();
		List<U32> defStamp(nv, 0);
		List<U32> ueStamp(nv, 0);
		List<List<U32>> defBlocks(nv);
		List<List<U32>> ueBlocks(nv);
		for(U32 b = 0; b < nb; ++b)
			for(const MachineInstr& in : fn.blocks[b].insts) {
				for(const MachineOperand& o : in.uses)
					if(o.isVReg() && defStamp[o.vreg] != b + 1 && ueStamp[o.vreg] != b + 1) {
						ueStamp[o.vreg] = b + 1;
						ueBlocks[o.vreg].push_back(b);
					}
				for(const MachineOperand& o : in.defs)
					if(o.isVReg() && defStamp[o.vreg] != b + 1) {
						defStamp[o.vreg] = b + 1;
						defBlocks[o.vreg].push_back(b);
					}
			}
		List<VReg> defIn(nb, kNoVReg);
		List<VReg> liveIn(nb, kNoVReg);
		List<VReg> outStamp(nb, kNoVReg);
		List<U32> work;
		liveOut.assign(nb, {});
		for(VReg v = 1; v < nv; ++v) {
			for(U32 b : defBlocks[v])
				defIn[b] = v;
			for(U32 b : ueBlocks[v]) {
				liveIn[b] = v;
				work.push_back(b);
			}
			while(!work.empty()) {
				U32 x = work.back();
				work.pop_back();
				for(I32 p : fn.blocks[x].preds) {
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
	void detail::RegAllocFunc::addSeg(VReg v, I32 start, I32 end) {
		List<RaSeg>& segs = iv[v].segs;
		if(!segs.empty() && end + 1 >= segs.back().first)
			segs.back().first = std::min(segs.back().first, start);
		else
			segs.emplace_back(start, end);
	}

	void detail::RegAllocFunc::noteCopy(const MachineInstr& in, U32 weight) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		if(d.isVReg() && s.isVReg() && fn.vregClass[d.vreg] == fn.vregClass[s.vreg])
			copies.push_back({~0u - weight, {d.vreg, s.vreg}});
		else if(d.isVReg() && s.isPhys())
			iv[d.vreg].hint = s.phys;
		else if(d.isPhys() && s.isVReg())
			iv[s.vreg].hint = d.phys;
	}

	// backward walk per block from its live-out set, blocks in reverse
	void detail::RegAllocFunc::buildIntervals() {
		iv.resize(nv);
		for(VReg v = 0; v < nv; ++v)
			iv[v].root = v;
		List<U8> live(nv, 0);
		List<I32> segEnd(nv, 0);
		List<VReg> liveList;
		for(U32 b = (U32)fn.blocks.size(); b-- > 0;) {
			const MachineBlock& blk = fn.blocks[b];
			if(blk.insts.empty())
				continue;
			for(VReg v : liveOut[b]) {
				live[v] = 1;
				segEnd[v] = 2 * (I32)blockFirst[b + 1] - 1;
				liveList.push_back(v);
			}
			U32 weight = 1;
			for(I32 d = blk.loopDepth; d > 0 && weight < detail::kMaxUseWeight; --d)
				weight *= detail::kLoopUseWeight;
			for(U32 k = (U32)blk.insts.size(); k-- > 0;) {
				const MachineInstr& in = blk.insts[k];
				I32 u = 2 * (I32)(blockFirst[b] + k);
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
				if(isCopy(in)) {
					useEnd = u;
					noteCopy(in, weight);
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
		for(RaInterval& t : iv)
			std::reverse(t.segs.begin(), t.segs.end());
	}

	// path halving
	VReg detail::RegAllocFunc::find(VReg v) {
		while(iv[v].root != v)
			v = iv[v].root = iv[iv[v].root].root;
		return v;
	}

	B32 detail::RegAllocFunc::overlaps(VReg a, VReg b) const {
		const List<RaSeg>& x = iv[a].segs;
		const List<RaSeg>& y = iv[b].segs;
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
	void detail::RegAllocFunc::merge(VReg a, VReg b) {
		RaInterval& t = iv[a];
		RaInterval& o = iv[b];
		List<RaSeg> segs(t.segs.size() + o.segs.size());
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
	void detail::RegAllocFunc::coalesce() {
		std::sort(copies.begin(), copies.end());
		for(const auto& [cold, pair] : copies) {
			VReg a = find(pair.first);
			VReg b = find(pair.second);
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

	PhysReg detail::RegAllocFunc::pick(VReg v) const {
		const RegClass& rc = ri.classes[fn.vregClass[v]];
		U64 blocked = ~allocMask[rc.id];
		for(const auto& [start, end] : iv[v].segs)
			for(I32 s = start; s <= end && blocked != ~0ull; ++s)
				blocked |= busy[(U64)s];
		PhysReg hint = iv[v].hint;
		if(hint != kNoReg && !((blocked >> hint) & 1))
			return hint;
		return detail::firstFree(rc.allocatable, blocked);
	}

	void detail::RegAllocFunc::assignRegs() {
		List<Pair<F32, VReg>> order; // (-weight, bundle)
		for(VReg v = 1; v < nv; ++v)
			if(!iv[v].segs.empty())
				order.emplace_back(-iv[v].weight, v);
		std::sort(order.begin(), order.end());
		for(const auto& [negWeight, v] : order) {
			RaInterval& t = iv[v];
			assert(!ri.classes[fn.vregClass[v]].scratch.empty() && "vreg of a class with no scratch");
			t.reg = pick(v);
			if(t.reg == kNoReg) {
				const RegClass& rc = ri.classes[fn.vregClass[v]];
				U32 bytes = rc.spillBytes;
				if(!bytes)
					bytes = ri.spillSlotBytes;
				t.slot = hooks.allocSlot(fn, rc.id, bytes);
				continue;
			}
			usedCallee |= ((U64)1 << t.reg) & calleeMask;
			for(const auto& [start, end] : t.segs)
				for(I32 s = start; s <= end; ++s)
					busy[(U64)s] |= (U64)1 << t.reg;
		}
	}

	PhysReg detail::RegAllocFunc::pickTemp(U32 cls, U64 hard, U64 soft) {
		const RegClass& rc = ri.classes[cls];
		PhysReg p = detail::firstFree(rc.scratch, hard | soft);
		if(p == kNoReg)
			p = detail::firstFree(rc.allocatable, hard | soft);
		if(p == kNoReg)
			p = detail::firstFree(rc.scratch, hard);
		assert(p != kNoReg && "no free register for a spilled operand");
		usedCallee |= ((U64)1 << p) & calleeMask;
		return p;
	}

	PhysReg
	detail::RegAllocFunc::spillReg(List<MachineInstr>& out, const MachineOperand& o, U32 i, B32 use) {
		VReg root = iv[o.vreg].root;
		for(const auto& [v, r] : temps)
			if(v == root)
				return r;
		U32 cls = fn.vregClass[o.vreg];
		U64 hard = (busy[2 * (U64)i] | busy[2 * (U64)i + 1]) & ~own;
		if(use)
			hard |= taken;
		PhysReg r = pickTemp(cls, hard, own | taken);
		if(use)
			out.push_back(hooks.makeReload(r, iv[root].slot, cls, o.width));
		taken |= (U64)1 << r;
		temps.emplace_back(root, r);
		return r;
	}

	void detail::RegAllocFunc::rewriteInstr(List<MachineInstr>& out, MachineInstr& in, U32 i) {
		temps.clear();
		taken = 0;
		own = 0;
		for(PhysReg p : in.clobbers)
			own |= (U64)1 << p;
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
			const RaInterval& t = bundle(o.vreg);
			PhysReg r = t.reg;
			if(r == kNoReg) {
				r = spillReg(out, o, i, false);
				stores.push_back(hooks.makeSpill(t.slot, r, fn.vregClass[o.vreg], o.width));
			}
			o = MachineOperand::fixed(r, o.width);
		}
		out.push_back(std::move(in));
		for(MachineInstr& s : stores)
			out.push_back(std::move(s));
	}

	// copies inside a bundle vanish
	void detail::RegAllocFunc::rewrite() {
		List<MachineInstr> out;
		for(U32 b = 0; b < fn.blocks.size(); ++b) {
			List<MachineInstr>& insts = fn.blocks[b].insts;
			out.clear();
			out.reserve(insts.size());
			for(U32 k = 0; k < insts.size(); ++k) {
				MachineInstr& in = insts[k];
				U32 i = blockFirst[b] + k;
				if(isCopy(in) && sameBundle(in.defs[0], in.uses[0]))
					continue;
				rewriteInstr(out, in, i);
			}
			insts.swap(out);
		}
	}

	B32 RegAllocPass::run(Module& module, MachineModule& mm, const TargetInfo& target) {
		B32 changed = false;
		RegAllocHooks hooks = target.regAllocHooks();
		for(const Function* f : module) {
			detail::RegAllocFunc(mm.get(f), *target.registers(), hooks).run();
			changed = true;
		}
		return changed;
	}
} // namespace rat
