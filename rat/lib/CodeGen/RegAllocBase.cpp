#include "CodeGen/RegAllocBase.h"

#include "CodeGen/MachineModule.h"
#include "IR/Module.h"
#include "Target/Target.h"

namespace rat {
	void RegAllocBase::number() {
		blkPts.assign(fn->blocks.size(), {});
		for(U32 b = 0; b < fn->blocks.size(); ++b)
			for(U32 i = 0; i < fn->blocks[b].insts.size(); ++i) {
				I32 pt = (I32)order.size();
				blkPts[b].push_back((U32)pt);
				order.push_back({b, i});
				const MachineInstr& in = fn->blocks[b].insts[i];
				if(in.isCall)
					callPts.push_back(pt);
				for(const MachineOperand& o : in.uses)
					if(o.isPhys())
						fixedAt[pt].insert(o.phys);
				for(const MachineOperand& o : in.defs)
					if(o.isPhys())
						fixedAt[pt].insert(o.phys);
				for(PhysReg p : in.clobbers)
					fixedAt[pt].insert(p);
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
					fixedAt[pt].insert(p);
					if(defsP)
						break;
				}
			}
		}
	}

	void RegAllocBase::liveness(List<Set<VReg>>& liveIn, List<Set<VReg>>& liveOut) {
		U32 nb = (U32)fn->blocks.size();
		List<Set<VReg>> useSet(nb), defSet(nb);
		for(U32 b = 0; b < nb; ++b) {
			Set<VReg> defined;
			for(const MachineInstr& in : fn->blocks[b].insts) {
				for(const MachineOperand& u : in.uses)
					if(u.isVReg() && !defined.count(u.vreg))
						useSet[b].insert(u.vreg);
				for(const MachineOperand& d : in.defs)
					if(d.isVReg()) {
						defined.insert(d.vreg);
						defSet[b].insert(d.vreg);
					}
			}
		}
		liveIn.assign(nb, {});
		liveOut.assign(nb, {});
		B32 changed = true;
		while(changed) {
			changed = false;
			for(I32 b = (I32)nb - 1; b >= 0; --b) {
				Set<VReg> out;
				for(I32 s : fn->blocks[b].succs)
					for(VReg v : liveIn[s])
						out.insert(v);
				Set<VReg> in = useSet[b];
				for(VReg v : out)
					if(!defSet[b].count(v))
						in.insert(v);
				if(in.size() != liveIn[b].size() || out.size() != liveOut[b].size()) {
					changed = true;
					liveIn[b] = std::move(in);
					liveOut[b] = std::move(out);
				}
			}
		}
	}

	U32 RegAllocBase::classOf(VReg v) const {
		auto it = fn->vregClass.find(v);
		return it == fn->vregClass.end() ? 0 : it->second;
	}

	const RegClass& RegAllocBase::regClass(U32 cls) const { return ri->classes[cls]; }

	B32 RegAllocBase::isCalleeSaved(const RegClass& rc, PhysReg p) {
		for(PhysReg c : rc.calleeSaved)
			if(c == p)
				return true;
		return false;
	}

	void RegAllocBase::rewrite() {
		for(U32 b = 0; b < fn->blocks.size(); ++b) {
			List<MachineInstr> out;
			for(MachineInstr& in : fn->blocks[b].insts) {
				U32 useScratch = 0;
				for(MachineOperand& u : in.uses) {
					if(!u.isVReg())
						continue;
					Assignment a = assignmentOf(u.vreg);
					if(a.spilled) {
						if(in.isCall) {
							u = MachineOperand::frameSlot(a.spillSlot, u.width);
							continue;
						}
						PhysReg sc = scratchAt(a.cls, useScratch++);
						out.push_back(hooks->makeReload(sc, a.spillSlot, a.cls, u.width));
						u = MachineOperand::fixed(sc, u.width);
					} else {
						u = MachineOperand::fixed(a.reg, u.width);
					}
				}

				U32 defScratch = 0;
				List<MachineInstr> spills;
				for(MachineOperand& d : in.defs) {
					if(!d.isVReg())
						continue;
					Assignment a = assignmentOf(d.vreg);
					if(a.spilled) {
						PhysReg sc = scratchAt(a.cls, defScratch++);
						spills.push_back(hooks->makeSpill(a.spillSlot, sc, a.cls, d.width));
						d = MachineOperand::fixed(sc, d.width);
					} else {
						d = MachineOperand::fixed(a.reg, d.width);
					}
				}

				out.push_back(in);
				for(MachineInstr& s : spills)
					out.push_back(std::move(s));
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
		ok = true;
		resetState();

		number();
		solve();
		rewrite();

		if(usedCalleeSaved) {
			usedCalleeSaved->clear();
			for(PhysReg p : usedCallee)
				usedCalleeSaved->push_back(p);
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
