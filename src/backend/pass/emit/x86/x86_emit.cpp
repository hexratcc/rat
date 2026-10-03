// the named x86 instruction emitters; one per X86Op, over a single plumbing
// function. every operand is materialized by the caller, so emission order is
// exactly call order.
#include "pass/emit/x86/x86_lower.h"

#include "codegen/machine_function.h"
#include "target/x86/x86_asm.h"

namespace rat {
	namespace detail {
		static MachineOperand vr(VReg v, U32 w = 8) { return MachineOperand::vr(v, w); }
		static MachineOperand ph(Reg r) { return MachineOperand::fixed(gpPhys(r)); }
		static MachineOperand xm(Xmm x) { return MachineOperand::fixed(xmmPhys(x.n), x.w); }
		static MachineOperand im(Imm i) { return MachineOperand::immVal(i.v, i.w); }
		static MachineOperand sl(Slot s) { return MachineOperand::frameSlot(s.s, s.w); }
	} // namespace detail

	using detail::im;
	using detail::kFp;
	using detail::kGp;
	using detail::kX87MemBits;
	using detail::ph;
	using detail::sl;
	using detail::vr;
	using detail::xm;

	MachineInstr& X86LowerPass::put(
			X86Op op, List<MachineOperand> defs, List<MachineOperand> uses, I64 imm, I64 imm2) {
		MachineInstr& m = mb->insts.emplace_back();
		m.op = (MachineOpcode)op;
		m.regClass = x86OpInfo(op).cls;
		m.defs = std::move(defs);
		m.uses = std::move(uses);
		m.imm = imm;
		m.imm2 = imm2;
		return m;
	}

	// data movement

	void X86LowerPass::mov(VReg d, VReg s) { put(X86Op::Copy, {vr(d)}, {vr(s)}); }
	void X86LowerPass::mov(Reg d, VReg s) { put(X86Op::Copy, {ph(d)}, {vr(s)}); }
	void X86LowerPass::mov(VReg d, Reg s) { put(X86Op::Copy, {vr(d)}, {ph(s)}); }
	void X86LowerPass::mov(Reg d, Reg s) { put(X86Op::Copy, {ph(d)}, {ph(s)}); }

	void X86LowerPass::mov(MachineOperand d, MachineOperand s, U32 cls) {
		put(X86Op::Copy, {std::move(d)}, {std::move(s)}).regClass = cls;
	}

	void X86LowerPass::movaps(VReg d, VReg s, U32 w) { mov(vr(d, w), vr(s, w), kFp); }
	void X86LowerPass::movaps(VReg d, Xmm s) { mov(vr(d, s.w), xm(s), kFp); }
	void X86LowerPass::movaps(Xmm d, VReg s) { mov(xm(d), vr(s, d.w), kFp); }

	void X86LowerPass::movi(VReg d, I64 v) {
		put(X86Op::LoadImm, {vr(d)}, {MachineOperand::immVal(v)});
	}

	void X86LowerPass::lea(VReg d, const String& s) {
		put(X86Op::LoadSym, {vr(d)}, {MachineOperand::symbol(s)});
	}

	void X86LowerPass::lea(VReg d, const AddrParts& a) {
		List<MachineOperand> uses = {addrBase(a), vr(a.index)};
		put(X86Op::Lea, {vr(d)}, std::move(uses), a.disp, a.scaleLog2 & 3);
	}

	void X86LowerPass::leaFrame(VReg d, I64 disp) { put(X86Op::FrameAddr, {vr(d)}, {}, disp); }

	void X86LowerPass::retAddr(VReg d) { put(X86Op::RetAddr, {vr(d)}, {}); }

	// integer memory

	List<MachineOperand> X86LowerPass::addrUses(const AddrParts& a) {
		List<MachineOperand> uses = {addrBase(a)};
		if(a.hasIndex)
			uses.push_back(vr(a.index));
		return uses;
	}

	void X86LowerPass::ld(VReg d, VReg base) { put(X86Op::Load, {vr(d)}, {vr(base)}); }

	void X86LowerPass::ld(VReg d, Slot s) { put(X86Op::Load, {vr(d)}, {sl(s)}); }

	void X86LowerPass::ld(VReg d, U32 w, const AddrParts& a, B32 sign) {
		put(X86Op::Load, {vr(d, w)}, addrUses(a), a.disp, sibBits(sign ? 1 : 0, a));
	}

	void X86LowerPass::ld(VReg d, U32 w, VReg base, B32 sign) {
		put(X86Op::Load, {vr(d, w)}, {vr(base)}, 0, sign ? 1 : 0);
	}

	void X86LowerPass::st(VReg base, MachineOperand src) {
		put(X86Op::Store, {}, {vr(base), std::move(src)});
	}

	void X86LowerPass::st(Slot d, Reg src) { put(X86Op::Store, {}, {sl(d), ph(src)}); }

	void X86LowerPass::st(const AddrParts& a, MachineOperand src) {
		List<MachineOperand> uses = {addrBase(a), std::move(src)};
		if(a.hasIndex)
			uses.push_back(vr(a.index));
		put(X86Op::Store, {}, std::move(uses), a.disp, sibBits(0, a));
	}

	// dynamic stack

	void X86LowerPass::stackAlloc(VReg d, VReg size) {
		// rounding runs through the scratch regs
		put(X86Op::StackAlloc, {vr(d)}, {vr(size)}).clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::stackSave(VReg d) { put(X86Op::StackSave, {vr(d)}, {}); }

	void X86LowerPass::stackRestore(VReg sp) { put(X86Op::StackRestore, {}, {vr(sp)}); }

	// non-local goto

	void X86LowerPass::setJmp() {
		MachineInstr& m = put(X86Op::SetJmp, {ph(RAX)}, {ph(R11)});
		m.isCall = true; // nothing survives the jump in a register
		m.clobbers = allRegClobbers();
	}

	void X86LowerPass::longJmp() { put(X86Op::LongJmp, {}, {ph(R11)}).clobbers = {gpReg(R10)}; }

	// integer ALU

	void X86LowerPass::alu(X86Op op, VReg d, VReg a, VReg b) { put(op, {vr(d)}, {vr(a), vr(b)}); }

	void X86LowerPass::alu(X86Op op, VReg d, VReg a, Imm b) { put(op, {vr(d)}, {vr(a), im(b)}); }

	void X86LowerPass::imul(VReg d, VReg a, Imm b) { put(X86Op::Mul, {vr(d)}, {vr(a), im(b)}); }

	void X86LowerPass::neg(VReg d) { put(X86Op::Neg, {vr(d)}, {vr(d)}); }
	void X86LowerPass::not_(VReg d) { put(X86Op::Not, {vr(d)}, {vr(d)}); }

	void X86LowerPass::shift(X86Op op, VReg d, Imm cnt) { put(op, {vr(d)}, {vr(d), im(cnt)}); }

	void X86LowerPass::shift(X86Op op, VReg d, Reg cl) { put(op, {vr(d)}, {vr(d), ph(cl)}); }

	void X86LowerPass::rot(X86Op op, VReg d, I64 cnt, U32 bits) {
		put(op, {vr(d)}, {vr(d), MachineOperand::immVal(cnt)}, (I64)bits);
	}

	void X86LowerPass::idiv(X86Op op, U32 bits) {
		List<MachineOperand> defs = {ph(RAX), ph(RDX)};
		put(op, std::move(defs), {ph(RAX), ph(RCX)}, (I64)bits);
	}

	void X86LowerPass::bitScan(X86Op op, VReg d, VReg s, U32 w) { put(op, {vr(d)}, {vr(s)}, (I64)w); }

	void X86LowerPass::cmp(VReg a, VReg b) { put(X86Op::Cmp, {}, {vr(a), vr(b)}); }
	void X86LowerPass::cmp(VReg a, Imm b) { put(X86Op::Cmp, {}, {vr(a), im(b)}); }

	void X86LowerPass::setcc(VReg d, U8 cc) { put(X86Op::SetCC, {vr(d)}, {}, (I64)cc); }

	void X86LowerPass::cmov(VReg d, VReg s, U8 cc) {
		put(X86Op::CMov, {vr(d)}, {vr(d), vr(s)}, (I64)cc);
	}

	void X86LowerPass::maskBitsOp(VReg d, U32 bits) {
		MachineInstr& m = put(X86Op::MaskBits, {vr(d)}, {vr(d)}, (I64)bits);
		if(bits > 32)
			m.clobbers = {gpReg(R11)}; // mask built via scratch reg
	}

	void X86LowerPass::signExtBitsOp(VReg d, U32 bits) {
		put(X86Op::SignExtBits, {vr(d)}, {vr(d)}, (I64)bits);
	}

	void X86LowerPass::bswap(VReg d, U32 w) { put(X86Op::Bswap, {vr(d)}, {vr(d)}, (I64)w); }

	// sse scalar float

	void X86LowerPass::ldf(VReg d, U32 w, VReg base) { put(X86Op::FLoad, {vr(d, w)}, {vr(base)}); }

	void X86LowerPass::ldf(VReg d, U32 w, const String& s) {
		put(X86Op::FLoad, {vr(d, w)}, {MachineOperand::symbol(s)});
	}

	void X86LowerPass::ldf(VReg d, U32 w, const AddrParts& a) {
		put(X86Op::FLoad, {vr(d, w)}, addrUses(a), a.disp, sibBits(0, a));
	}

	void X86LowerPass::stf(const AddrParts& a, VReg s, U32 w) {
		List<MachineOperand> uses = {addrBase(a), vr(s, w)};
		if(a.hasIndex)
			uses.push_back(vr(a.index));
		put(X86Op::FStore, {}, std::move(uses), a.disp, sibBits(0, a));
	}

	void X86LowerPass::farith(X86Op op, VReg d, VReg a, VReg b, U32 w, I64 desc) {
		movaps(d, a, w);
		put(op, {vr(d, w)}, {vr(d, w), vr(b, w)}, desc);
	}

	void X86LowerPass::fneg(VReg d, VReg s, U32 w) {
		// the encoder builds 0-x through the top volatile xmm
		put(X86Op::FNeg, {vr(d, w)}, {vr(s, w)}, (I64)w).clobbers = {
				xmmReg(conv->sseVolatileCount - 1)};
	}

	void X86LowerPass::fsqrt(VReg d, VReg s, U32 w) {
		put(X86Op::FSqrt, {vr(d, w)}, {vr(s, w)}, (I64)w);
	}

	void X86LowerPass::fabs_(VReg d, VReg s, U32 w) {
		put(X86Op::FAbs, {vr(d, w)}, {vr(s, w)}, (I64)w);
	}

	void X86LowerPass::ucomis(VReg d, VReg a, VReg b, U32 w, U8 cc, B32 swap) {
		List<MachineOperand> uses = {vr(a, w), vr(b, w)};
		put(X86Op::FCmp, {vr(d)}, std::move(uses), (I64)cc, swap ? 1 : 0);
	}

	void X86LowerPass::ucomisFlags(VReg a, VReg b, U32 w, B32 swap) {
		put(X86Op::FCmpFlags, {}, {vr(a, w), vr(b, w)}, 0, swap ? 1 : 0);
	}

	void X86LowerPass::cvtf(VReg d, U32 dw, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide) {
		put(X86Op::Cvt, {vr(d, dw)}, {vr(s, sw)}, cvtDesc(pfx, opc, wide));
	}

	void X86LowerPass::cvti(VReg d, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide) {
		put(X86Op::Cvt, {vr(d)}, {vr(s, sw)}, cvtDesc(pfx, opc, wide)).regClass = kGp;
	}

	// sse packed vector

	void X86LowerPass::varith(VReg d, VReg a, VReg b, U8 pfx, U8 opc, B32 esc38) {
		I64 desc = ((I64)(esc38 ? 1 : 0) << 16) | ((I64)pfx << 8) | opc;
		farith(X86Op::VArith, d, a, b, 16, desc);
	}

	void X86LowerPass::vsplat(VReg d, VReg s, U32 esz, B32 isInt) {
		List<MachineOperand> uses = {vr(s, isInt ? 8 : esz)};
		put(X86Op::VSplat, {vr(d, 16)}, std::move(uses), (I64)esz, isInt ? 1 : 0);
	}

	void X86LowerPass::vextract(VReg d, VReg s, U32 lane, U32 esz, B32 isInt) {
		I64 desc = ((I64)esz << 1) | (isInt ? 1 : 0);
		MachineInstr& m = put(X86Op::VExtract, {vr(d, isInt ? 8 : esz)}, {vr(s, 16)}, (I64)lane, desc);
		m.regClass = isInt ? kGp : kFp;
	}

	void X86LowerPass::vpackMem(VReg d, const List<MachineOperand>& lanes, U32 esz, B32 isInt) {
		for(U32 i = 0; i < (U32)lanes.size(); ++i) {
			I64 desc = ((I64)i << 1) | (isInt ? 1 : 0);
			put(X86Op::VPackLane, {}, {lanes[i]}, (I64)esz, desc).regClass = isInt ? kGp : kFp;
		}
		put(X86Op::VPack, {vr(d, 16)}, {}, (I64)esz, isInt ? 1 : 0);
	}

	void X86LowerPass::vpackReg(VReg d, const List<MachineOperand>& lanes, U32 esz) {
		put(X86Op::VPackReg, {vr(d, 16)}, {lanes[0]}, (I64)esz, 1);
		for(U32 i = 1; i < (U32)lanes.size(); ++i) {
			List<MachineOperand> uses = {vr(d, 16), lanes[i]};
			put(X86Op::VInsertReg, {vr(d, 16)}, std::move(uses), (I64)esz, (I64)i);
		}
	}

	void X86LowerPass::vshuf(VReg d, VReg s, U8 sel) {
		put(X86Op::VShuf, {vr(d, 16)}, {vr(s, 16)}, (I64)sel);
	}

	// x87

	void X86LowerPass::fld(Slot d, VReg addr) {
		put(X86Op::X87LoadMem, {sl(d)}, {vr(addr)}, kX87MemBits);
	}

	void X86LowerPass::fldPop(Slot s) { put(X86Op::X87LoadMem, {}, {sl(s)}, -1); }

	void X86LowerPass::fstp(VReg addr, Slot s) {
		put(X86Op::X87StoreMem, {}, {vr(addr), sl(s)}, kX87MemBits);
	}

	void X86LowerPass::fstp(Slot d) { put(X86Op::X87StoreMem, {sl(d)}, {}, -1); }

	void X86LowerPass::fstpDiscard() { put(X86Op::X87StoreMem, {}, {}, -2); }

	void X86LowerPass::fldImm(Slot d, U64 bits) {
		put(X86Op::X87LoadImmD, {sl(d)}, {MachineOperand::immVal((I64)bits)});
	}

	void X86LowerPass::fild(Slot d, VReg s) { put(X86Op::X87FromInt, {sl(d)}, {vr(s)}); }

	void X86LowerPass::fistp(VReg d, Slot s) { put(X86Op::X87ToInt, {vr(d)}, {sl(s)}); }

	void X86LowerPass::fldSse(Slot d, VReg s, U32 w) {
		put(X86Op::X87FromSse, {sl(d)}, {vr(s, w)}, (I64)w);
	}

	void X86LowerPass::fldSlot(Slot d, Slot s) {
		put(X86Op::X87FromSse, {sl(d)}, {sl(s)}, kX87MemBits);
	}

	void X86LowerPass::fstpSse(VReg d, U32 w, Slot s) {
		put(X86Op::X87ToSse, {vr(d, w)}, {sl(s)}, (I64)w);
	}

	void X86LowerPass::x87Arith(X86Op op, Slot d, Slot a, Slot b) {
		put(op, {sl(d)}, {sl(a), sl(b)});
	}

	void X86LowerPass::fchs(Slot d, Slot s) { put(X86Op::X87Neg, {sl(d)}, {sl(s)}); }

	void X86LowerPass::fucomi(VReg d, Slot a, Slot b, U8 cc, B32 swap) {
		put(X86Op::X87Cmp, {vr(d)}, {sl(a), sl(b)}, (I64)cc, swap ? 1 : 0);
	}

	// control

	void X86LowerPass::jmp(I32 target) { put(X86Op::Jmp, {}, {MachineOperand::blockRef(target)}); }

	// imm2 = 1: condition code in imm, no predicate register
	void X86LowerPass::jcc(U8 cc, I32 thenB, I32 elseB) {
		List<MachineOperand> uses = {MachineOperand::blockRef(thenB), MachineOperand::blockRef(elseB)};
		put(X86Op::Br, {}, std::move(uses), (I64)cc, 1);
	}

	void X86LowerPass::br(VReg pred, I32 thenB, I32 elseB) {
		List<MachineOperand> uses = {
				vr(pred), MachineOperand::blockRef(thenB), MachineOperand::blockRef(elseB)};
		put(X86Op::Br, {}, std::move(uses));
	}

	void X86LowerPass::switchJump(VReg sel, const List<I32>& targets) {
		List<MachineOperand> uses = {vr(sel)};
		for(I32 t : targets)
			uses.push_back(MachineOperand::blockRef(t));
		put(X86Op::SwitchJump, {}, std::move(uses)).clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::vaStart(VReg ptr, U32 namedGp, U32 namedFp) {
		MachineInstr& m = put(X86Op::VaStart, {}, {vr(ptr)}, (I64)namedGp, (I64)namedFp);
		m.clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::vaArg(MachineOperand def, VReg ptr, VaArgKind kind, I64 desc, U32 cls) {
		MachineInstr& m = put(X86Op::VaArg, {std::move(def)}, {vr(ptr)}, (I64)kind, desc);
		m.regClass = cls;
		m.clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::ud2() { put(X86Op::Ud2, {}, {}); }

	void X86LowerPass::prefetch(VReg addr, U8 hint) {
		put(X86Op::Prefetch, {}, {vr(addr)}, 0, (I64)hint);
	}
} // namespace rat
