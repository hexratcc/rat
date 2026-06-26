#include "CodeGen/RegAlloc.h"

#include "Target/Target.h"

namespace rat {
	namespace {
		struct Interval {
			VReg vreg = kNoVReg;
			U32 cls = 0;
			I32 start = -1;
			I32 end = -1;
			PhysReg assigned = kNoReg;
			I32 spillSlot = 0;
			B32 spilled = false;
			B32 crossesCall = false;
		};

		struct Scan {
			MachineFunc& fn;
			const RegisterInfo& ri;
			const RegAllocHooks& hooks;

			struct Loc {
				U32 block;
				U32 inst;
			};

			List<Loc> order;
			List<List<U32>> blkPts;
			List<I32> callPts;
			Map<I32, Set<PhysReg>> fixedAt;
			Map<VReg, Interval> intervals;
			Set<PhysReg> usedCallee;
			B32 ok = true;

			Scan(MachineFunc& f, const RegisterInfo& r, const RegAllocHooks& h)
			: fn(f),
				ri(r),
				hooks(h) {}

			void number() {
				blkPts.assign(fn.blocks.size(), {});
				for(U32 b = 0; b < fn.blocks.size(); ++b)
					for(U32 i = 0; i < fn.blocks[b].insts.size(); ++i) {
						I32 pt = (I32)order.size();
						blkPts[b].push_back((U32)pt);
						order.push_back({b, i});
						const MachineInstr& in = fn.blocks[b].insts[i];
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

			void pinFixedArgWindows() {
				for(I32 c : callPts) {
					U32 b = order[(U32)c].block;
					U32 callIdx = order[(U32)c].inst;
					const List<U32>& pts = blkPts[b];
					const MachineInstr& call = fn.blocks[b].insts[callIdx];
					for(const MachineOperand& u : call.uses) {
						if(!u.isPhys())
							continue;
						PhysReg p = u.phys;
						for(I32 i = (I32)callIdx - 1; i >= 0; --i) {
							I32 pt = (I32)pts[(U32)i];
							const MachineInstr& in = fn.blocks[b].insts[(U32)i];
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

			void liveness(List<Set<VReg>>& liveIn, List<Set<VReg>>& liveOut) {
				U32 nb = (U32)fn.blocks.size();
				List<Set<VReg>> useSet(nb), defSet(nb);
				for(U32 b = 0; b < nb; ++b) {
					Set<VReg> defined;
					for(const MachineInstr& in : fn.blocks[b].insts) {
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
						for(I32 s : fn.blocks[b].succs)
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

			void extend(VReg v, U32 cls, I32 point) {
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

			U32 classOf(VReg v) const {
				auto it = fn.vregClass.find(v);
				return it == fn.vregClass.end() ? 0 : it->second;
			}

			void buildIntervals() {
				List<Set<VReg>> liveIn, liveOut;
				liveness(liveIn, liveOut);

				for(U32 b = 0; b < fn.blocks.size(); ++b) {
					if(blkPts[b].empty())
						continue;
					I32 first = (I32)blkPts[b].front();
					I32 last = (I32)blkPts[b].back();
					for(VReg v : liveIn[b])
						extend(v, classOf(v), first);
					for(VReg v : liveOut[b])
						extend(v, classOf(v), last);

					for(U32 i = 0; i < fn.blocks[b].insts.size(); ++i) {
						I32 pt = (I32)blkPts[b][i];
						const MachineInstr& in = fn.blocks[b].insts[i];
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

			const RegClass& regClass(U32 cls) const { return ri.classes[cls]; }

			Set<PhysReg> forbidden(const Interval& iv) const {
				Set<PhysReg> bad;
				for(const auto& kv : fixedAt)
					if(iv.start <= kv.first && kv.first <= iv.end)
						for(PhysReg p : kv.second)
							bad.insert(p);
				return bad;
			}

			void allocate() {
				List<Interval*> sorted;
				for(auto& kv : intervals)
					sorted.push_back(&kv.second);
				std::sort(sorted.begin(), sorted.end(), [](const Interval* a, const Interval* b) {
					return a->start < b->start;
				});

				List<Interval*> active;
				Map<U32, Set<PhysReg>> freeRegs;
				for(const RegClass& rc : ri.classes)
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

			static B32 isCalleeSaved(const RegClass& rc, PhysReg p) {
				for(PhysReg c : rc.calleeSaved)
					if(c == p)
						return true;
				return false;
			}

			void spillAt(Interval* cur, List<Interval*>& active) {
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
					victim->spillSlot = hooks.allocSlot(fn, victim->cls, ri.spillSlotBytes);
					for(Interval*& a : active)
						if(a == victim)
							a = cur;
				} else {
					cur->spilled = true;
					cur->spillSlot = hooks.allocSlot(fn, cur->cls, ri.spillSlotBytes);
				}
			}

			void rewrite() {
				for(U32 b = 0; b < fn.blocks.size(); ++b) {
					List<MachineInstr> out;
					for(MachineInstr& in : fn.blocks[b].insts) {
						U32 useScratch = 0;
						for(MachineOperand& u : in.uses) {
							if(!u.isVReg())
								continue;
							Interval& iv = intervals[u.vreg];
							if(iv.spilled) {
								if(in.isCall) {
									u = MachineOperand::frameSlot(iv.spillSlot, u.width);
									continue;
								}
								PhysReg sc = scratchAt(iv.cls, useScratch++);
								out.push_back(hooks.makeReload(sc, iv.spillSlot, iv.cls, u.width));
								u = MachineOperand::fixed(sc, u.width);
							} else {
								u = MachineOperand::fixed(iv.assigned, u.width);
							}
						}

						U32 defScratch = 0;
						List<MachineInstr> spills;
						for(MachineOperand& d : in.defs) {
							if(!d.isVReg())
								continue;
							Interval& iv = intervals[d.vreg];
							if(iv.spilled) {
								PhysReg sc = scratchAt(iv.cls, defScratch++);
								spills.push_back(hooks.makeSpill(iv.spillSlot, sc, iv.cls, d.width));
								d = MachineOperand::fixed(sc, d.width);
							} else {
								d = MachineOperand::fixed(iv.assigned, d.width);
							}
						}

						out.push_back(in);
						for(MachineInstr& s : spills)
							out.push_back(std::move(s));
					}
					fn.blocks[b].insts = std::move(out);
				}
			}

			PhysReg scratchAt(U32 cls, U32 idx) {
				const RegClass& rc = ri.classes[cls];
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

			B32 run() {
				number();
				buildIntervals();
				allocate();
				rewrite();
				return ok;
			}
		};
	} // namespace

	B32 allocateRegisters(MachineFunc& fn,
												const RegisterInfo& ri,
												const RegAllocHooks& hooks,
												List<PhysReg>* usedCalleeSaved) {
		Scan s(fn, ri, hooks);
		B32 r = s.run();
		if(usedCalleeSaved) {
			usedCalleeSaved->clear();
			for(PhysReg p : s.usedCallee)
				usedCalleeSaved->push_back(p);
		}
		return r;
	}

	U32 RegAllocPass::runOnMachineFunction(const Function&,
																				 MachineFunc& mf,
																				 const TargetInfo& target) {
		B32 allocated =
				allocateRegisters(mf, *target.registers(), target.regAllocHooks(), &mf.usedCalleeSaved);
		assert(allocated && "register allocation ran out of scratch registers");
		(void)allocated;
		return 1;
	}
} // namespace rat
