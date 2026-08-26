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

	B32 X86PeepholePass::run(Module& module, MachineModule& mm, const TargetInfo&) {
		U32 changed = 0;
		for(const Function* fn : module) {
			MachineFunc& mf = mm.get(fn);
			for(MachineBlock& b : mf.blocks)
				if(b.id >= 0)
					changed += runOnBlock(b);
		}
		return changed != 0;
	}
} // namespace rat
