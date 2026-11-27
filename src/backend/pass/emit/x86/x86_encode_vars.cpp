#include "pass/emit/x86/x86_encode.h"

#include "codegen/machine_function.h"
#include "ir/function.h"

// debug var locations in the style of LLVM's LiveDebugValues, a block starts with the locations all
// its predecessors agree on, a record moves its var, a write ends the vars in its location.
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
			if(d.isFrameSlot() && d.slot == slot)
				return true;
		X86Op op = (X86Op)in.op;
		if(in.isCall || op == X86Op::Load || op == X86Op::FLoad || op == X86Op::Copy)
			return false;
		for(const MachineOperand& u : in.uses)
			if(u.isFrameSlot() && u.slot == slot)
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
		if(it == live.end() || it->inst != inst) {
			if(loc.kind != VarLoc::Kind::None)
				live.insert(it, {inst, loc, pc});
			return;
		}
		if(it->loc == loc)
			return;
		closeRange(*it, pc);
		if(loc.kind == VarLoc::Kind::None)
			live.erase(it);
		else
			*it = {inst, loc, pc};
	}

	void X86EncodePass::applyRecords(VarState& vars, U32 chain, U32 pc) {
		for(U32 r = chain; r; r = fn->dbg[r].next) {
			const DbgRec& rec = fn->dbg[r];
			VarLoc loc = toVarLoc(rec.loc);
			B32 stale = std::count(vars.stale.begin(), vars.stale.end(), loc) != 0;
			if(rec.inst != kDbgKill)
				setLoc(vars.live, rec.inst, stale ? VarLoc{} : loc, pc);
			else if(!stale)
				vars.stale.push_back(loc);
		}
	}

	// a write makes its location hold a value again
	void X86EncodePass::freshen(List<VarLoc>& stale, const MachineInstr& in) {
		for(const MachineOperand& d : in.defs)
			stale.erase(std::remove(stale.begin(), stale.end(), toVarLoc(d)), stale.end());
		if(detail::isAnySlotStore(in))
			stale.erase(std::remove(stale.begin(), stale.end(), toVarLoc(in.uses[0])), stale.end());
	}

	// the epilogue ends all
	void X86EncodePass::clobber(VarState& vars, const MachineInstr& in, U32 pc) {
		freshen(vars.stale, in);
		if(vars.live.empty())
			return;
		B32 ret = (X86Op)in.op == X86Op::Ret;
		U64 regs = in.clobbers;
		for(const MachineOperand& d : in.defs)
			if(d.isPhys())
				regs |= detail::physBit(d.phys);
		U32 kept = 0;
		for(const OpenVarRange& open : vars.live) {
			B32 regWritten = open.loc.kind == VarLoc::Kind::Reg && ((regs >> open.loc.reg) & 1);
			B32 slotWritten = open.loc.kind == VarLoc::Kind::Frame && writesSlot(in, open.loc.value);
			if(ret || regWritten || slotWritten)
				closeRange(open, pc);
			else
				vars.live[kept++] = open;
		}
		vars.live.resize(kept);
	}

	// the locations both sides agree on, the stale slots of either. true when into changed
	B32 X86EncodePass::meet(VarState& into, const VarState& from) {
		U32 kept = 0;
		U32 j = 0;
		for(const OpenVarRange& open : into.live) {
			while(j < from.live.size() && from.live[j].inst < open.inst)
				++j;
			if(j < from.live.size() && from.live[j].inst == open.inst && from.live[j].loc == open.loc)
				into.live[kept++] = open;
		}
		B32 changed = kept != into.live.size();
		into.live.resize(kept);
		for(const VarLoc& loc : from.stale)
			if(!std::count(into.stale.begin(), into.stale.end(), loc)) {
				into.stale.push_back(loc);
				changed = true;
			}
		return changed;
	}

	// emitting also encodes the block, solving passes no pcs
	void X86EncodePass::walkBlock(U32 bi, VarState& vars, B32 emit) {
		const MachineBlock& blk = fn->blocks[bi];
		B32 track = !varsIn.empty();
		U32 pc = emit ? a->here() : kNoPc;
		if(track)
			vars = varsIn[bi];
		for(OpenVarRange& open : vars.live)
			open.start = pc;
		I32 fallthrough = -1;
		if(bi + 1 < fn->blocks.size())
			fallthrough = fn->blocks[bi + 1].id;
		for(const MachineInstr& in : blk.insts) {
			U32 at = emit ? a->here() : kNoPc;
			// a write ends its vars one byte in, so the instr start still shows them but no later pc does
			U32 writeEnd = kNoPc;
			if(track)
				applyRecords(vars, in.dbg, at);
			if(emit) {
				markLine(in.loc);
				emitInst(in, fallthrough);
				markScope(in.loc, at, a->here());
				writeEnd = std::min(at + 1, a->here());
			}
			if(track)
				clobber(vars, in, writeEnd);
		}
		pc = emit ? a->here() : kNoPc;
		if(track)
			applyRecords(vars, blk.dbgEnd, pc);
		for(const OpenVarRange& open : vars.live)
			closeRange(open, pc);
	}

	// like gcc's max-vartrack-size, huge functions keep their vars without locations
	void X86EncodePass::trackVars(U32 instCount) {
		varsIn.clear();
		if(!encodedFunc || fn->dbg.size() <= 1)
			return;
		const DebugBody* body = encodedFunc->fn->getDebug();
		if(body && (U64)instCount * body->vars.size() <= kMaxVarTrackCost)
			solveVars();
	}

	// in-states only lose locations and gain stale slots, the first predecessor seeds them
	// succs still name the blocks forwarded jumps skip, so their records count
	void X86EncodePass::solveVars() {
		U32 nb = (U32)fn->blocks.size();
		varsIn.assign(nb, {});
		List<I32> at(detail::blockIdBound(*fn), -1);
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
		reached[entry] = 1;
		dirty[entry] = 1;
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
					B32 changed = true;
					if(reached[(U32)s])
						changed = meet(varsIn[(U32)s], out);
					else
						varsIn[(U32)s] = out;
					reached[(U32)s] = 1;
					dirty[(U32)s] |= changed;
					again |= changed;
				}
			}
		}
	}
} // namespace rat
