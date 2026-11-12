#include "pass/emit/x86/x86_encode.h"

#include "codegen/machine_function.h"

// debug var locations in the style of LLVM's LiveDebugValues, a block starts with the locations all
// its predecessors agree on, a record moves its var, a write to a location ends the vars living there.
// a kill record poisons its location, poison flows along the values computed from it and a var is
// not placed where it is poisoned
namespace rat {
	VarLoc X86EncodePass::toVarLoc(const MachineOperand& o) {
		switch(o.kind) {
		case MachineOperand::Kind::Phys:
			return {VarLoc::Kind::Reg, o.phys, 0};
		case MachineOperand::Kind::FrameSlot:
			return {VarLoc::Kind::Frame, 0, (I64)o.slot - detail::kCfaFromRbp};
		case MachineOperand::Kind::Imm:
			return {VarLoc::Kind::Const, 0, o.imm};
		default:
			return {};
		}
	}

	// loads and call stack arguments only read their slot, anything else may write it
	B32 X86EncodePass::writesSlot(const MachineInstr& in, I64 cfaOff) {
		I32 slot = (I32)(cfaOff + detail::kCfaFromRbp);
		for(const MachineOperand& d : in.defs)
			if(d.kind == MachineOperand::Kind::FrameSlot && d.slot == slot)
				return true;
		X86Op op = (X86Op)in.op;
		if(in.isCall || op == X86Op::Load || op == X86Op::FLoad || op == X86Op::Copy)
			return false;
		for(const MachineOperand& u : in.uses)
			if(u.kind == MachineOperand::Kind::FrameSlot && u.slot == slot)
				return true;
		return false;
	}

	B32 X86EncodePass::OpenVarRange::operator<(U32 other) const { return inst < other; }

	void X86EncodePass::closeRange(const OpenVarRange& open, U32 end) {
		if(end == kNoPc || end <= open.start)
			return;
		VarLoc loc = open.loc;
		if(loc.kind == VarLoc::Kind::Reg)
			loc.reg = detail::kDwarfRegs[loc.reg];
		List<VarRange>& r = encodedFunc->varRanges[open.inst];
		if(!r.empty() && r.back().end == open.start && r.back().loc == loc)
			r.back().end = end;
		else
			r.push_back({open.start, end, loc});
	}

	void X86EncodePass::setLoc(List<OpenVarRange>& live, U32 inst, VarLoc loc, U32 pc) {
		auto it = std::lower_bound(live.begin(), live.end(), inst);
		B32 found = it != live.end() && it->inst == inst;
		if(found && it->loc == loc)
			return;
		if(found)
			closeRange(*it, pc);
		if(found && loc.kind == VarLoc::Kind::None)
			live.erase(it);
		else if(found)
			*it = {inst, loc, pc};
		else if(loc.kind != VarLoc::Kind::None)
			live.insert(it, {inst, loc, pc});
	}

	U32 X86EncodePass::okBits(const List<Poison>& poison, const VarLoc& loc) {
		for(const Poison& p : poison)
			if(p.loc == loc)
				return p.okBits;
		return kClean;
	}

	void X86EncodePass::setPoison(List<Poison>& poison, const VarLoc& loc, U32 ok) {
		U32 i = 0;
		while(i < poison.size() && !(poison[i].loc == loc))
			++i;
		if(ok == kClean && i < poison.size())
			poison.erase(poison.begin() + i);
		else if(ok == kClean)
			return;
		else if(i < poison.size())
			poison[i].okBits = ok;
		else
			poison.push_back({loc, ok});
	}

	void X86EncodePass::applyRecords(VarState& vars, U32 chain, U32 pc) {
		for(U32 r = chain; r; r = fn->dbg[r].next) {
			const DbgRec& rec = fn->dbg[r];
			VarLoc loc = toVarLoc(rec.loc);
			U32 ok = okBits(vars.poison, loc);
			if(rec.inst == kDbgKill)
				setPoison(vars.poison, loc, std::min(ok, (U32)rec.loc.width));
			else
				setLoc(vars.live, rec.inst, ok < rec.loc.width * 8u ? VarLoc{} : loc, pc);
		}
	}

	// the right low bits of what in computes from its poisoned inputs, kClean when none is. a slot store
	// passes its source on, a normalize within the right bits makes it right again
	U32 X86EncodePass::poisonFlow(const List<Poison>& poison, const MachineInstr& in) {
		B32 store = detail::isAnySlotStore(in);
		U32 ok = kClean;
		U32 first = store ? 1 : 0; // a slot store writes uses[0]
		for(U32 i = first; i < in.uses.size(); ++i)
			ok = std::min(ok, okBits(poison, toVarLoc(in.uses[i])));
		X86Op op = (X86Op)in.op;
		if(ok == kClean || store)
			return ok;
		if(!(x86OpInfo(op).flags & detail::kX86KeepsLowBits))
			return 0;
		if(detail::isNormalize(op) && (U64)in.imm <= ok)
			return kClean;
		return ok;
	}

	// a partial write keeps the poison of the bits it leaves
	void
	X86EncodePass::writePoison(List<Poison>& poison, const MachineOperand& o, B32 partial, U32 flow) {
		VarLoc loc = toVarLoc(o);
		U32 left = partial ? okBits(poison, loc) : kClean;
		setPoison(poison, loc, std::min(left, flow));
	}

	void X86EncodePass::passPoison(List<Poison>& poison, const MachineInstr& in) {
		if(poison.empty())
			return;
		U32 flow = poisonFlow(poison, in);
		U32 kept = 0;
		for(const Poison& p : poison)
			if(p.loc.kind != VarLoc::Kind::Reg || !((in.clobbers >> p.loc.reg) & 1))
				poison[kept++] = p;
		poison.resize(kept);
		for(const MachineOperand& d : in.defs)
			if(d.isPhys() || d.kind == MachineOperand::Kind::FrameSlot)
				writePoison(poison, d, d.width < 4, flow);
		if(detail::isAnySlotStore(in))
			writePoison(poison, in.uses[0], in.uses[1].width < 8, flow);
	}

	// the epilogue ends all
	void X86EncodePass::clobber(VarState& vars, const MachineInstr& in, U32 pc) {
		passPoison(vars.poison, in);
		if(vars.live.empty())
			return;
		B32 ret = (X86Op)in.op == X86Op::Ret;
		U64 regs = in.clobbers;
		for(const MachineOperand& d : in.defs)
			if(d.isPhys())
				regs |= detail::physBit(d.phys);
		U32 kept = 0;
		for(const OpenVarRange& open : vars.live) {
			B32 reg = open.loc.kind == VarLoc::Kind::Reg;
			B32 frame = open.loc.kind == VarLoc::Kind::Frame;
			B32 written =
					(reg && (regs >> open.loc.reg) & 1) || (frame && writesSlot(in, open.loc.value));
			if(ret || written)
				closeRange(open, pc);
			else
				vars.live[kept++] = open;
		}
		vars.live.resize(kept);
	}

	// the locations both sides agree on, the poison of either. true when into changed
	B32 X86EncodePass::meet(VarState& into, const VarState& from) {
		U32 before = (U32)into.live.size();
		U32 kept = 0;
		U32 j = 0;
		for(const OpenVarRange& open : into.live) {
			while(j < from.live.size() && from.live[j].inst < open.inst)
				++j;
			if(j < from.live.size() && from.live[j].inst == open.inst && from.live[j].loc == open.loc)
				into.live[kept++] = open;
		}
		into.live.resize(kept);
		B32 changed = kept != before;
		for(const Poison& p : from.poison)
			if(p.okBits < okBits(into.poison, p.loc)) {
				setPoison(into.poison, p.loc, p.okBits);
				changed = true;
			}
		return changed;
	}

	// the var state through block bi, from its in-state when tracked. emitting also encodes its code,
	// solving passes no pcs
	void X86EncodePass::walkBlock(U32 bi, VarState& vars, B32 emit) {
		const MachineBlock& blk = fn->blocks[bi];
		B32 track = !varsIn.empty();
		U32 pc = emit ? a->here() : kNoPc;
		if(track)
			vars = varsIn[bi];
		for(OpenVarRange& open : vars.live)
			open.start = pc;
		I32 fallthrough = bi + 1 < fn->blocks.size() ? fn->blocks[bi + 1].id : -1;
		for(const MachineInstr& in : blk.insts) {
			U32 at = emit ? a->here() : kNoPc;
			if(emit)
				markLine(in.loc);
			if(track)
				applyRecords(vars, in.dbg, at);
			if(emit) {
				emitInst(in, fallthrough);
				markScope(in.loc, at, a->here());
			}
			// a write ends its vars one byte in, so the instr start still shows them but no later pc does
			if(track)
				clobber(vars, in, emit ? std::min(at + 1, a->here()) : kNoPc);
		}
		pc = emit ? a->here() : kNoPc;
		if(track)
			applyRecords(vars, blk.dbgEnd, pc);
		for(const OpenVarRange& open : vars.live)
			closeRange(open, pc);
	}

	// in-states only shrink in locations and grow in poison, a block is redone when its in-state
	// changed. the first predecessor to reach a block seeds it. the successor lists still name the
	// blocks forwarded jumps skip, so their records count
	void X86EncodePass::solveVars() {
		U32 nb = (U32)fn->blocks.size();
		varsIn.assign(nb, {});
		List<I32> at(blockIdBound(*fn), -1);
		for(U32 bi = 0; bi < nb; ++bi)
			if(fn->blocks[bi].id >= 0)
				at[(U32)fn->blocks[bi].id] = (I32)bi;
		U32 entry = 0;
		while(entry < nb && fn->blocks[entry].id < 0)
			++entry;
		if(entry == nb)
			return;
		List<U8> reached(nb, 0);
		List<U8> dirty(nb, 0);
		reached[entry] = dirty[entry] = 1;
		VarState out;
		for(B32 again = true; again;) {
			again = false;
			for(U32 bi = 0; bi < nb; ++bi) {
				if(!dirty[bi])
					continue;
				dirty[bi] = 0;
				walkBlock(bi, out, false);
				for(I32 sid : fn->blocks[bi].succs) {
					I32 s = at[(U32)sid];
					if(s < 0)
						continue;
					B32 changed = !reached[(U32)s] || meet(varsIn[(U32)s], out);
					if(!reached[(U32)s])
						varsIn[(U32)s] = out;
					reached[(U32)s] = 1;
					dirty[(U32)s] |= changed;
					again |= changed;
				}
			}
		}
	}
} // namespace rat
