#include "CodeGen/LinearScanRegAlloc.h"

#include "Target/Target.h"

namespace rat {
	void LinearScanRegAllocPass::solve() {
		buildIntervals();
		assignRegs();
	}

	RegAllocBase::Assignment LinearScanRegAllocPass::assignmentOf(VReg v) {
		const Interval& iv = intervals[v];
		return {iv.assigned, iv.spillSlot, iv.cls, iv.spilled};
	}

	void LinearScanRegAllocPass::extend(VReg v, U32 cls, I32 point) {
		Interval& iv = intervals[v];
		if(iv.vreg == kNoVReg) {
			iv.vreg = v;
			iv.cls = cls;
			iv.start = point;
			iv.end = point;
		} else {
			if(point < iv.start)
				iv.start = point;
			if(point > iv.end)
				iv.end = point;
		}
	}

	void LinearScanRegAllocPass::buildIntervals() {
		List<Set<VReg>> liveIn, liveOut;
		liveness(liveIn, liveOut);

		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			if(blkPts[b].empty())
				continue;
			I32 first = (I32)blkPts[b].front();
			I32 last = (I32)blkPts[b].back();
			for(VReg v : liveIn[b])
				extend(v, classOf(v), first);
			for(VReg v : liveOut[b])
				extend(v, classOf(v), last);

			for(U32 i = 0; i < fn->blocks[b].insts.size(); ++i) {
				I32 pt = (I32)blkPts[b][i];
				const MachineInstr& in = fn->blocks[b].insts[i];
				for(const MachineOperand& u : in.uses)
					if(u.isVReg())
						extend(u.vreg, classOf(u.vreg), pt);
				for(const MachineOperand& d : in.defs)
					if(d.isVReg())
						extend(d.vreg, classOf(d.vreg), pt);
			}
		}

		for(auto& kv : intervals) {
			Interval& iv = kv.second;
			for(I32 c : callPts)
				if(iv.start < c && c < iv.end) {
					iv.crossesCall = true;
					break;
				}
		}
	}

	Set<PhysReg> LinearScanRegAllocPass::forbidden(const Interval& iv) const {
		Set<PhysReg> bad;
		for(const auto& kv : fixedAt)
			if(iv.start <= kv.first && kv.first <= iv.end)
				for(PhysReg p : kv.second)
					bad.insert(p);
		return bad;
	}

	void LinearScanRegAllocPass::assignRegs() {
		List<Interval*> sorted;
		for(auto& kv : intervals)
			sorted.push_back(&kv.second);
		std::sort(sorted.begin(), sorted.end(), [](const Interval* a, const Interval* b) {
			return a->start < b->start;
		});

		List<Interval*> active;
		Map<U32, Set<PhysReg>> freeRegs;
		for(const RegClass& rc : ri->classes)
			for(PhysReg p : rc.allocatable)
				freeRegs[rc.id].insert(p);

		auto expire = [&](I32 start) {
			List<Interval*> stillActive;
			for(Interval* a : active) {
				if(a->end < start) {
					if(!a->spilled && a->assigned != kNoReg)
						freeRegs[a->cls].insert(a->assigned);
				} else
					stillActive.push_back(a);
			}
			active = std::move(stillActive);
		};

		for(Interval* iv : sorted) {
			expire(iv->start);

			const RegClass& rc = regClass(iv->cls);
			Set<PhysReg>& pool = freeRegs[iv->cls];
			Set<PhysReg> bad = forbidden(*iv);
			PhysReg pick = kNoReg;
			if(iv->crossesCall) {
				for(PhysReg p : rc.calleeSaved)
					if(pool.count(p) && !bad.count(p)) {
						pick = p;
						break;
					}
			} else {
				// short-lived value
				for(PhysReg p : rc.allocatable)
					if(pool.count(p) && !bad.count(p)) {
						pick = p;
						break;
					}
			}

			if(pick != kNoReg) {
				pool.erase(pick);
				iv->assigned = pick;
				if(isCalleeSaved(rc, pick))
					usedCallee.insert(pick);
				active.push_back(iv);
			} else {
				spillAt(iv, active);
			}
		}
	}

	void LinearScanRegAllocPass::spillAt(Interval* cur, List<Interval*>& active) {
		const RegClass& rc = regClass(cur->cls);
		Set<PhysReg> bad = forbidden(*cur);
		Interval* victim = nullptr;
		for(Interval* a : active)
			if(a->cls == cur->cls && !a->spilled) {
				if(cur->crossesCall && !isCalleeSaved(rc, a->assigned))
					continue;
				if(bad.count(a->assigned))
					continue;
				if(!victim || a->end > victim->end)
					victim = a;
			}

		if(victim && victim->end > cur->end) {
			cur->assigned = victim->assigned;
			if(isCalleeSaved(rc, cur->assigned))
				usedCallee.insert(cur->assigned);
			victim->assigned = kNoReg;
			victim->spilled = true;
			victim->spillSlot = hooks->allocSlot(*fn, victim->cls, ri->spillSlotBytes);
			for(Interval*& a : active)
				if(a == victim)
					a = cur;
		} else {
			cur->spilled = true;
			cur->spillSlot = hooks->allocSlot(*fn, cur->cls, ri->spillSlotBytes);
		}
	}

} // namespace rat
