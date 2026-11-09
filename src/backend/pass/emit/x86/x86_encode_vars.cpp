#include "pass/emit/x86/x86_encode.h"

#include "codegen/machine_function.h"

// debug var locations in the style of LLVM's LiveDebugValues, a block starts with the locations all
// its predecessors agree on, a record moves its var, a write to a location ends the vars living there
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

	B32 X86EncodePass::instBefore(const OpenVarRange& open, U32 inst) { return open.inst < inst; }

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
		auto it = std::lower_bound(live.begin(), live.end(), inst, instBefore);
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

	void X86EncodePass::applyRecords(List<OpenVarRange>& live, U32 chain, U32 pc) {
		for(U32 r = chain; r; r = fn->dbg[r].next)
			setLoc(live, fn->dbg[r].inst, toVarLoc(fn->dbg[r].loc), pc);
	}

	// the epilogue ends all
	void X86EncodePass::clobber(List<OpenVarRange>& live, const MachineInstr& in, U32 pc) {
		if(live.empty())
			return;
		B32 ret = (X86Op)in.op == X86Op::Ret;
		U64 regs = in.clobbers;
		for(const MachineOperand& d : in.defs)
			if(d.isPhys())
				regs |= detail::physBit(d.phys);
		U32 kept = 0;
		for(const OpenVarRange& open : live) {
			B32 reg = open.loc.kind == VarLoc::Kind::Reg;
			B32 frame = open.loc.kind == VarLoc::Kind::Frame;
			B32 written = (reg && (regs >> open.loc.reg) & 1) || (frame && writesSlot(in, open.loc.value));
			if(ret || written)
				closeRange(open, pc);
			else
				live[kept++] = open;
		}
		live.resize(kept);
	}

	// what both sides agree on
	void X86EncodePass::meet(List<OpenVarRange>& into, const List<OpenVarRange>& from) {
		U32 kept = 0;
		U32 j = 0;
		for(const OpenVarRange& open : into) {
			while(j < from.size() && from[j].inst < open.inst)
				++j;
			if(j < from.size() && from[j].inst == open.inst && from[j].loc == open.loc)
				into[kept++] = open;
		}
		into.resize(kept);
	}

	void X86EncodePass::blockVars(const MachineBlock& blk, List<OpenVarRange>& live) {
		for(const MachineInstr& in : blk.insts) {
			applyRecords(live, in.dbg, kNoPc);
			clobber(live, in, kNoPc);
		}
		applyRecords(live, blk.dbgEnd, kNoPc);
	}

	// in-states only shrink, a block is redone when its in-state changed. the first predecessor to
	// reach a block seeds it. the successor lists still name the blocks forwarded jumps skip, so their
	// records count
	void X86EncodePass::solveVars(const MachineFunc& f) {
		U32 nb = (U32)f.blocks.size();
		varsIn.assign(nb, {});
		List<I32> at(blockIdBound(f), -1);
		for(U32 bi = 0; bi < nb; ++bi)
			if(f.blocks[bi].id >= 0)
				at[(U32)f.blocks[bi].id] = (I32)bi;
		U32 entry = 0;
		while(entry < nb && f.blocks[entry].id < 0)
			++entry;
		if(entry == nb)
			return;
		List<U8> reached(nb, 0);
		List<U8> dirty(nb, 0);
		reached[entry] = dirty[entry] = 1;
		List<OpenVarRange> out;
		for(B32 again = true; again;) {
			again = false;
			for(U32 bi = 0; bi < nb; ++bi) {
				if(!dirty[bi])
					continue;
				dirty[bi] = 0;
				out = varsIn[bi];
				blockVars(f.blocks[bi], out);
				for(I32 sid : f.blocks[bi].succs) {
					I32 s = at[(U32)sid];
					if(s < 0)
						continue;
					U32 before = (U32)varsIn[(U32)s].size();
					if(!reached[(U32)s])
						varsIn[(U32)s] = out;
					else
						meet(varsIn[(U32)s], out);
					B32 changed = !reached[(U32)s] || varsIn[(U32)s].size() != before;
					reached[(U32)s] = 1;
					dirty[(U32)s] |= changed;
					again |= changed;
				}
			}
		}
	}

	void X86EncodePass::beginVars(U32 bi) {
		liveVars = varsIn[bi];
		for(OpenVarRange& open : liveVars)
			open.start = a->here();
	}

	void X86EncodePass::endVars(const MachineBlock& blk) {
		applyRecords(liveVars, blk.dbgEnd, a->here());
		for(const OpenVarRange& open : liveVars)
			closeRange(open, a->here());
	}
} // namespace rat
