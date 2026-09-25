#include "codegen/reg_alloc_base.h"

#include "codegen/machine_module.h"
#include "ir/module.h"
#include "target/target.h"

namespace rat {
	void RegAllocBase::number() {
		blkPts.assign(fn->blocks.size(), {});
		for(U32 b = 0; b < fn->blocks.size(); ++b)
			for(U32 i = 0; i < fn->blocks[b].insts.size(); ++i) {
				I32 pt = (I32)order.size();
				blkPts[b].push_back((U32)pt);
				order.push_back({b, i});
				fixedAt.push_back(0);
				const MachineInstr& in = fn->blocks[b].insts[i];
				if(in.isCall)
					callPts.push_back(pt);
				for(const MachineOperand& o : in.uses)
					if(o.isPhys())
						fixedAt[pt] |= (U64)1 << o.phys;
				for(const MachineOperand& o : in.defs)
					if(o.isPhys())
						fixedAt[pt] |= (U64)1 << o.phys;
				for(PhysReg p : in.clobbers)
					fixedAt[pt] |= (U64)1 << p;
			}
		pinFixedArgWindows();
	}

	void RegAllocBase::pinFixedArgWindows() {
		for(I32 c : callPts) {
			U32 b = order[(U32)c].block;
			U32 callIdx = order[(U32)c].inst;
			const List<U32>& pts = blkPts[b];
			const MachineInstr& call = fn->blocks[b].insts[callIdx];
			for(const MachineOperand& u : call.uses) {
				if(!u.isPhys())
					continue;
				PhysReg p = u.phys;
				for(I32 i = (I32)callIdx - 1; i >= 0; --i) {
					I32 pt = (I32)pts[(U32)i];
					const MachineInstr& in = fn->blocks[b].insts[(U32)i];
					B32 defsP = false;
					for(const MachineOperand& d : in.defs)
						if(d.isPhys() && d.phys == p) {
							defsP = true;
							break;
						}
					fixedAt[pt] |= (U64)1 << p;
					if(defsP)
						break;
				}
			}
		}
	}

	void RegAllocBase::collectCopyHints() {
		if(!hooks->isCopy)
			return;
		for(U32 b = 0; b < (U32)fn->blocks.size(); ++b)
			for(U32 i = 0; i < (U32)fn->blocks[b].insts.size(); ++i) {
				const MachineInstr& in = fn->blocks[b].insts[i];
				if(!hooks->isCopy(in) || in.defs.size() != 1 || in.uses.size() != 1)
					continue;
				const MachineOperand& d = in.defs[0];
				const MachineOperand& u = in.uses[0];
				I32 pt = (I32)blkPts[b][i];
				if(d.isVReg() && u.isVReg()) {
					if(classOf(d.vreg) != classOf(u.vreg))
						continue;
					copyHints[d.vreg].push_back({u.vreg, pt});
					copyHints[u.vreg].push_back({d.vreg, pt});
				} else if(d.isVReg() && u.isPhys()) {
					physHints[d.vreg].push_back(u.phys);
					copyPinAt[d.vreg].emplace(pt, u.phys);
				} else if(d.isPhys() && u.isVReg()) {
					physHints[u.vreg].push_back(d.phys);
					copyPinAt[u.vreg].emplace(pt, d.phys);
				}
			}
	}

	List<PhysReg> RegAllocBase::hintedRegs(VReg v, const Delegate<PhysReg(VReg)>& colorOf) const {
		List<PhysReg> hints;
		if(auto it = physHints.find(v); it != physHints.end())
			for(PhysReg p : it->second)
				hints.push_back(p);
		if(auto it = copyHints.find(v); it != copyHints.end())
			for(const CopyHint& h : it->second)
				if(PhysReg p = colorOf(h.partner); p != kNoReg)
					hints.push_back(p);
		return hints;
	}

	List<I32> RegAllocBase::copyPointsBetween(VReg a, VReg b) const {
		List<I32> pts;
		if(auto it = copyHints.find(a); it != copyHints.end())
			for(const CopyHint& h : it->second)
				if(h.partner == b)
					pts.push_back(h.pt);
		return pts;
	}

	I32 RegAllocBase::takeSpillSlot(U32 cls, I32 start, I32 end) {
		for(PooledSlot& ps : slotPool[cls])
			if(ps.freeEnd < start) {
				ps.freeEnd = end;
				return ps.slot;
			}
		U32 bytes = ri->spillSlotBytes;
		for(const RegClass& rc : ri->classes)
			if(rc.id == cls && rc.spillBytes)
				bytes = rc.spillBytes;
		I32 slot = hooks->allocSlot(*fn, cls, bytes);
		slotPool[cls].push_back({slot, end});
		return slot;
	}

	void DenseLive::prep(U32 nb, U32 nv) {
		for(List<VRegSet>* v : {&in, &out, &use, &def}) {
			if(v->size() < nb)
				v->resize(nb);
			for(U32 i = 0; i < nb; ++i)
				(*v)[i].resetAll(nv);
		}
	}

	void SparseLive::prep(U32 nb) {
		for(List<VRegList>* v : {&in, &out, &use, &def}) {
			if(v->size() < nb)
				v->resize(nb);
			for(U32 i = 0; i < nb; ++i)
				(*v)[i].clear();
		}
	}

	void detail::vregUnion(const VRegList& a, const VRegList& b, VRegList& dst) {
		dst.clear();
		U32 i = 0, j = 0;
		while(i < a.size() && j < b.size()) {
			if(a[i] < b[j])
				dst.push_back(a[i++]);
			else if(b[j] < a[i])
				dst.push_back(b[j++]);
			else {
				dst.push_back(a[i++]);
				++j;
			}
		}
		while(i < a.size())
			dst.push_back(a[i++]);
		while(j < b.size())
			dst.push_back(b[j++]);
	}

	void detail::vregUnionMasked(const VRegList& a,
															 const VRegList& b,
															 const VRegList& mask,
															 VRegList& dst) {
		dst.clear();
		U32 i = 0, m = 0;
		for(U32 j = 0; j < b.size(); ++j) {
			while(m < mask.size() && mask[m] < b[j])
				++m;
			if(m < mask.size() && mask[m] == b[j]) // killed by a def in this block
				continue;
			while(i < a.size() && a[i] < b[j])
				dst.push_back(a[i++]);
			if(i < a.size() && a[i] == b[j])
				++i;
			dst.push_back(b[j]);
		}
		while(i < a.size())
			dst.push_back(a[i++]);
	}

	void RegAllocBase::blockUseDefsSparse() {
		VRegSet used(fn->nextVReg), defd(fn->nextVReg);
		for(U32 b = 0; b < (U32)fn->blocks.size(); ++b) {
			VRegList& use = sparseLive.use[b];
			VRegList& def = sparseLive.def[b];
			for(const MachineInstr& in : fn->blocks[b].insts) {
				for(const MachineOperand& u : in.uses)
					if(u.isVReg() && !defd.test(u.vreg) && !used.test(u.vreg)) {
						used.set(u.vreg);
						use.push_back(u.vreg);
					}
				for(const MachineOperand& d : in.defs)
					if(d.isVReg() && !defd.test(d.vreg)) {
						defd.set(d.vreg);
						def.push_back(d.vreg);
					}
			}
			for(VReg v : use)
				used.reset(v);
			for(VReg v : def)
				defd.reset(v);
			std::sort(use.begin(), use.end());
			std::sort(def.begin(), def.end());
		}
	}

	void RegAllocBase::liveOutOf(U32 b, VRegList& out, VRegList& tmp) {
		const List<I32>& succs = fn->blocks[b].succs;
		if(succs.empty()) {
			out.clear();
			return;
		}
		out = sparseLive.in[(U32)succs[0]];
		for(U32 i = 1; i < (U32)succs.size(); ++i) {
			detail::vregUnion(out, sparseLive.in[(U32)succs[i]], tmp);
			out.swap(tmp);
		}
	}

	constexpr U64 kDenseLivenessBytes = 64ull << 20;

	B32 RegAllocBase::denseLivenessFits() const {
		U64 words = (fn->nextVReg + 63) / 64;
		U64 perBlock = words * 8 * 4; // in, out, use and def, eight bytes to the word
		return (U64)fn->blocks.size() * perBlock <= kDenseLivenessBytes;
	}

	void RegAllocBase::liveness() {
		liveIsDense = denseLivenessFits();
		if(liveIsDense)
			livenessDense();
		else
			livenessSparse();
	}

	void RegAllocBase::blockUseDefsDense() {
		for(U32 b = 0; b < (U32)fn->blocks.size(); ++b)
			for(const MachineInstr& in : fn->blocks[b].insts) {
				for(const MachineOperand& u : in.uses)
					if(u.isVReg() && !denseLive.def[b].test(u.vreg))
						denseLive.use[b].set(u.vreg);
				for(const MachineOperand& d : in.defs)
					if(d.isVReg())
						denseLive.def[b].set(d.vreg);
			}
	}

	void RegAllocBase::livenessDense() {
		U32 nb = (U32)fn->blocks.size();
		U32 nv = fn->nextVReg;
		denseLive.prep(nb, nv);
		blockUseDefsDense();
		B32 changed = true;
		VRegSet out, in;
		out.resetAll(nv);
		in.resetAll(nv);
		while(changed) {
			changed = false;
			for(I32 b = (I32)nb - 1; b >= 0; --b) {
				out.resetAll(nv);
				for(I32 s : fn->blocks[b].succs)
					out.orWith(denseLive.in[s]);
				in.assignUnionMasked(denseLive.use[b], out, denseLive.def[b]); // use | (out & ~def)
				if(!(in == denseLive.in[b]) || !(out == denseLive.out[b])) {
					changed = true;
					denseLive.in[b].copyFrom(in);
					denseLive.out[b].copyFrom(out);
				}
			}
		}
	}

	void RegAllocBase::livenessSparse() {
		U32 nb = (U32)fn->blocks.size();
		sparseLive.prep(nb);
		blockUseDefsSparse();
		B32 changed = true;
		VRegList out, in, tmp;
		while(changed) {
			changed = false;
			for(I32 b = (I32)nb - 1; b >= 0; --b) {
				liveOutOf((U32)b, out, tmp);
				// use | (out & ~def)
				detail::vregUnionMasked(sparseLive.use[b], out, sparseLive.def[b], in);
				if(in != sparseLive.in[b] || out != sparseLive.out[b]) {
					changed = true;
					sparseLive.in[b] = in;
					sparseLive.out[b] = out;
				}
			}
		}
	}

	U32 RegAllocBase::classOf(VReg v) const {
		return v < fn->vregClass.size() ? fn->vregClass[v] : 0;
	}

	const RegClass& RegAllocBase::regClass(U32 cls) const { return ri->classes[cls]; }

	B32 RegAllocBase::isCalleeSaved(const RegClass& rc, PhysReg p) {
		for(PhysReg c : rc.calleeSaved)
			if(c == p)
				return true;
		return false;
	}

	void RegAllocBase::collectRematDefs() {
		if(!hooks->isRemat)
			return;
		Map<VReg, U32> defCount;
		for(U32 b = 0; b < fn->blocks.size(); ++b)
			for(const MachineInstr& in : fn->blocks[b].insts)
				for(const MachineOperand& d : in.defs)
					if(d.isVReg())
						++defCount[d.vreg];
		for(U32 b = 0; b < fn->blocks.size(); ++b)
			for(const MachineInstr& in : fn->blocks[b].insts) {
				if(in.defs.size() != 1 || !in.defs[0].isVReg() || !hooks->isRemat(in))
					continue;
				VReg v = in.defs[0].vreg;
				if(defCount[v] == 1)
					rematDef[v] = in;
			}
	}

	B32 RegAllocBase::dropsRematDef(const MachineInstr& in) {
		if(in.defs.size() != 1 || !in.defs[0].isVReg())
			return false;
		VReg v = in.defs[0].vreg;
		return rematDef.count(v) && assignmentOf(v).spilled && !slotReadByCall.count(v);
	}

	void RegAllocBase::emitReload(List<MachineInstr>& out,
																PhysReg dst,
																const MachineOperand& u,
																const Assignment& a) {
		auto rt = rematDef.find(u.vreg);
		if(rt == rematDef.end()) {
			out.push_back(hooks->makeReload(dst, a.spillSlot, a.cls, u.width));
			return;
		}
		MachineInstr m = rt->second;
		m.defs[0] = MachineOperand::fixed(dst, u.width);
		out.push_back(std::move(m));
	}

	void RegAllocBase::emitStore(List<MachineInstr>& out, I32 slot, PhysReg src, U32 cls, U32 width) {
		out.push_back(hooks->makeSpill(slot, src, cls, width));
		memo = {true, slot, src, cls, width};
	}

	// the register that just stored its slot or target after a reload into it
	PhysReg RegAllocBase::sourceOf(List<MachineInstr>& out,
																 const MachineOperand& u,
																 const Assignment& a,
																 PhysReg target) {
		if(memo.on && memo.cls == a.cls && memo.slot == a.spillSlot && memo.width == u.width)
			return memo.reg;
		emitReload(out, target, u, a);
		return target;
	}

	B32 RegAllocBase::rewriteCopy(List<MachineInstr>& out, MachineInstr& in) {
		MachineOperand& d = in.defs[0];
		MachineOperand& u = in.uses[0];
		Assignment da = d.isVReg() ? assignmentOf(d.vreg) : Assignment{};
		Assignment ua = u.isVReg() ? assignmentOf(u.vreg) : Assignment{};
		if(!da.spilled && !ua.spilled)
			return false;
		if(da.spilled && ua.spilled && da.spillSlot == ua.spillSlot)
			return true;				// slot self-copy
		PhysReg dst = kNoReg; // where the value must land
		if(d.isPhys())
			dst = d.phys;
		else if(!da.spilled)
			dst = da.reg;
		PhysReg src;
		if(ua.spilled)
			src = sourceOf(out, u, ua, dst != kNoReg ? dst : scratchAt(ua.cls, 0));
		else
			src = u.isPhys() ? u.phys : ua.reg;
		if(da.spilled) {
			emitStore(out, da.spillSlot, src, da.cls, d.width);
			return true;
		}
		if(src != dst) {
			d = MachineOperand::fixed(dst, d.width);
			u = MachineOperand::fixed(src, u.width);
			out.push_back(in);
		}
		memo.on = false;
		return true;
	}

	void RegAllocBase::rewriteInstr(List<MachineInstr>& out, MachineInstr& in, I32 pt) {
		U32 useScratch[kMaxRegClasses] = {0};
		for(MachineOperand& u : in.uses) {
			if(!u.isVReg())
				continue;
			Assignment a = assignmentOf(u.vreg);
			if(!a.spilled) {
				u = MachineOperand::fixed(a.reg, u.width);
				continue;
			}
			if(in.isCall) {
				u = MachineOperand::frameSlot(a.spillSlot, u.width);
				continue;
			}
			if(Piece* pc = pieceAt(u.vreg, pt)) {
				if(!pc->loaded)
					emitReload(out, pc->reg, u, a);
				pc->loaded = true;
				u = MachineOperand::fixed(pc->reg, u.width);
				continue;
			}
			// the previous instruction just stored this slot from a register nothing has
			// touched since, reuse
			B32 tied = false;
			for(const MachineOperand& d : in.defs)
				if(d.isVReg() && d.vreg == u.vreg)
					tied = true;
			if(memo.on && !tied && useScratch[a.cls] == 0 && memo.cls == a.cls &&
				 memo.slot == a.spillSlot && memo.width == u.width) {
				u = MachineOperand::fixed(memo.reg, u.width);
				++useScratch[a.cls]; // reserve index 0 in case memo.reg is scratch 0
				continue;
			}
			PhysReg sc = scratchAt(a.cls, useScratch[a.cls]++);
			emitReload(out, sc, u, a);
			u = MachineOperand::fixed(sc, u.width);
		}

		U32 defScratch[kMaxRegClasses] = {0};
		List<MachineInstr> stores;
		for(MachineOperand& d : in.defs) {
			if(!d.isVReg())
				continue;
			Assignment a = assignmentOf(d.vreg);
			if(!a.spilled) {
				d = MachineOperand::fixed(a.reg, d.width);
				continue;
			}
			PhysReg sc;
			if(Piece* pc = pieceAt(d.vreg, pt)) {
				sc = pc->reg;
				pc->loaded = true;
			} else {
				sc = scratchAt(a.cls, defScratch[a.cls]++);
			}
			stores.push_back(hooks->makeSpill(a.spillSlot, sc, a.cls, d.width));
			d = MachineOperand::fixed(sc, d.width);
		}

		memo.on = false;
		out.push_back(in);
		for(MachineInstr& s : stores)
			out.push_back(std::move(s));
		if(stores.empty() || in.isCall)
			return;
		// uses[0] = frame slot, uses[1] = source register
		const MachineInstr& last = out.back();
		if(last.uses.size() == 2 && last.uses[0].kind == MachineOperand::Kind::FrameSlot &&
			 last.uses[1].kind == MachineOperand::Kind::Phys)
			memo = {true, last.uses[0].slot, last.uses[1].phys, last.regClass, last.uses[1].width};
	}

	void RegAllocBase::rewrite() {
		slotReadByCall.clear();
		for(U32 b = 0; b < fn->blocks.size(); ++b)
			for(const MachineInstr& in : fn->blocks[b].insts)
				if(in.isCall)
					for(const MachineOperand& u : in.uses)
						if(u.isVReg())
							slotReadByCall.insert(u.vreg);

		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			List<MachineInstr> out;
			memo.on = false;
			for(U32 i = 0; i < fn->blocks[b].insts.size(); ++i) {
				MachineInstr& in = fn->blocks[b].insts[i];
				I32 pt = (I32)blkPts[b][i];
				if(dropsRematDef(in)) { // every use remats instead
					memo.on = false;
					continue;
				}
				B32 copy = hooks->isCopy && hooks->isCopy(in) && in.defs.size() == 1 && in.uses.size() == 1;
				if(copy && rewriteCopy(out, in))
					continue;
				rewriteInstr(out, in, pt);
			}
			fn->blocks[b].insts = std::move(out);
		}
	}

	PhysReg RegAllocBase::scratchAt(U32 cls, U32 idx) {
		const RegClass& rc = ri->classes[cls];
		if(rc.scratch.empty()) {
			ok = false;
			return kNoReg;
		}
		if(idx >= rc.scratch.size()) {
			ok = false;
			idx = (U32)rc.scratch.size() - 1;
		}
		return rc.scratch[idx];
	}

	B32 RegAllocBase::allocate(MachineFunc& f,
														 const RegisterInfo& r,
														 const RegAllocHooks& h,
														 List<PhysReg>* usedCalleeSaved) {
		fn = &f;
		ri = &r;
		hooks = &h;
		order.clear();
		blkPts.clear();
		callPts.clear();
		fixedAt.clear();
		usedCallee.clear();
		copyHints.clear();
		physHints.clear();
		copyPinAt.clear();
		slotPool.clear();
		rematDef.clear();
		ok = true;
		resetState();

		number();
		collectCopyHints();
		collectRematDefs();

		solve();
		assignPieces();
		rewrite();

		if(usedCalleeSaved) {
			usedCalleeSaved->assign(usedCallee.begin(), usedCallee.end());
			std::sort(usedCalleeSaved->begin(), usedCalleeSaved->end());
		}
		return ok;
	}

	B32 RegAllocBase::run(Module& module, MachineModule& mm, const TargetInfo& target) {
		U32 changed = 0;
		for(const Function* fn : module) {
			MachineFunc& mf = mm.get(fn);
			B32 allocated =
					allocate(mf, *target.registers(), target.regAllocHooks(), &mf.usedCalleeSaved);
			assert(allocated && "register allocation ran out of scratch registers");
			(void)allocated;
			++changed;
		}
		return changed != 0;
	}
} // namespace rat
