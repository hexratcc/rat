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

	MachineInstr& X86LowerPass::put(
			X86Op op, List<MachineOperand> defs, List<MachineOperand> uses, I64 imm, I64 imm2) {
		MachineInstr m;
		m.op = (MachineOpcode)op;
		m.regClass = x86OpInfo(op).cls;
		m.defs = std::move(defs);
		m.uses = std::move(uses);
		m.imm = imm;
		m.imm2 = imm2;
		mb->insts.push_back(std::move(m));
		return mb->insts.back();
	}

	// data movement

	void X86LowerPass::mov(VReg d, VReg s) { put(X86Op::Copy, {detail::vr(d)}, {detail::vr(s)}); }
	void X86LowerPass::mov(Reg d, VReg s) { put(X86Op::Copy, {detail::ph(d)}, {detail::vr(s)}); }
	void X86LowerPass::mov(VReg d, Reg s) { put(X86Op::Copy, {detail::vr(d)}, {detail::ph(s)}); }
	void X86LowerPass::mov(Reg d, Reg s) { put(X86Op::Copy, {detail::ph(d)}, {detail::ph(s)}); }

	void X86LowerPass::mov(MachineOperand d, MachineOperand s, U32 cls) {
		put(X86Op::Copy, {std::move(d)}, {std::move(s)}).regClass = cls;
	}

	void X86LowerPass::movaps(VReg d, VReg s, U32 w) {
		mov(detail::vr(d, w), detail::vr(s, w), detail::kFp);
	}
	void X86LowerPass::movaps(VReg d, Xmm s) { mov(detail::vr(d, s.w), detail::xm(s), detail::kFp); }
	void X86LowerPass::movaps(Xmm d, VReg s) { mov(detail::xm(d), detail::vr(s, d.w), detail::kFp); }

	void X86LowerPass::movi(VReg d, I64 v) {
		put(X86Op::LoadImm, {detail::vr(d)}, {MachineOperand::immVal(v)});
	}

	void X86LowerPass::lea(VReg d, const String& s) {
		put(X86Op::LoadSym, {detail::vr(d)}, {MachineOperand::symbol(s)});
	}

	void X86LowerPass::lea(VReg d, const AddrParts& a) {
		List<MachineOperand> uses = {addrBase(a), detail::vr(a.index)};
		put(X86Op::Lea, {detail::vr(d)}, std::move(uses), a.disp, a.scaleLog2 & 3);
	}

	void X86LowerPass::leaFrame(VReg d, I64 disp) {
		put(X86Op::FrameAddr, {detail::vr(d)}, {}, disp);
	}

	void X86LowerPass::retAddr(VReg d) { put(X86Op::RetAddr, {detail::vr(d)}, {}); }

	// integer memory

	List<MachineOperand> X86LowerPass::addrUses(const AddrParts& a) {
		List<MachineOperand> uses = {addrBase(a)};
		if(a.hasIndex)
			uses.push_back(detail::vr(a.index));
		return uses;
	}

	void X86LowerPass::ld(VReg d, VReg base) {
		put(X86Op::Load, {detail::vr(d)}, {detail::vr(base)});
	}

	void X86LowerPass::ld(VReg d, Slot s) { put(X86Op::Load, {detail::vr(d)}, {detail::sl(s)}); }

	void X86LowerPass::ld(VReg d, U32 w, const AddrParts& a, B32 sign) {
		put(X86Op::Load, {detail::vr(d, w)}, addrUses(a), a.disp, sibBits(sign ? 1 : 0, a));
	}

	void X86LowerPass::ld(VReg d, U32 w, VReg base, B32 sign) {
		put(X86Op::Load, {detail::vr(d, w)}, {detail::vr(base)}, 0, sign ? 1 : 0);
	}

	void X86LowerPass::st(VReg base, MachineOperand src) {
		put(X86Op::Store, {}, {detail::vr(base), std::move(src)});
	}

	void X86LowerPass::st(Slot d, Reg src) {
		put(X86Op::Store, {}, {detail::sl(d), detail::ph(src)});
	}

	void X86LowerPass::st(const AddrParts& a, MachineOperand src) {
		List<MachineOperand> uses = {addrBase(a), std::move(src)};
		if(a.hasIndex)
			uses.push_back(detail::vr(a.index));
		put(X86Op::Store, {}, std::move(uses), a.disp, sibBits(0, a));
	}

	// dynamic stack

	void X86LowerPass::stackAlloc(VReg d, VReg size) {
		// rounding runs through the scratch regs
		put(X86Op::StackAlloc, {detail::vr(d)}, {detail::vr(size)}).clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::stackSave(VReg d) { put(X86Op::StackSave, {detail::vr(d)}, {}); }

	void X86LowerPass::stackRestore(VReg sp) { put(X86Op::StackRestore, {}, {detail::vr(sp)}); }

	// non-local goto

	void X86LowerPass::setJmp() {
		MachineInstr& m = put(X86Op::SetJmp, {detail::ph(RAX)}, {detail::ph(R11)});
		m.isCall = true; // nothing survives the jump in a register
		m.clobbers = allRegClobbers();
	}

	void X86LowerPass::longJmp() {
		put(X86Op::LongJmp, {}, {detail::ph(R11)}).clobbers = {gpReg(R10)};
	}

	// integer ALU

	void X86LowerPass::alu(X86Op op, VReg d, VReg a, VReg b) {
		put(op, {detail::vr(d)}, {detail::vr(a), detail::vr(b)});
	}

	void X86LowerPass::alu(X86Op op, VReg d, VReg a, Imm b) {
		put(op, {detail::vr(d)}, {detail::vr(a), detail::im(b)});
	}

	void X86LowerPass::imul(VReg d, VReg a, Imm b) {
		put(X86Op::Mul, {detail::vr(d)}, {detail::vr(a), detail::im(b)});
	}

	void X86LowerPass::neg(VReg d) { put(X86Op::Neg, {detail::vr(d)}, {detail::vr(d)}); }
	void X86LowerPass::not_(VReg d) { put(X86Op::Not, {detail::vr(d)}, {detail::vr(d)}); }

	void X86LowerPass::shift(X86Op op, VReg d, Imm cnt) {
		put(op, {detail::vr(d)}, {detail::vr(d), detail::im(cnt)});
	}

	void X86LowerPass::shift(X86Op op, VReg d, Reg cl) {
		put(op, {detail::vr(d)}, {detail::vr(d), detail::ph(cl)});
	}

	void X86LowerPass::rot(X86Op op, VReg d, I64 cnt, U32 bits) {
		put(op, {detail::vr(d)}, {detail::vr(d), MachineOperand::immVal(cnt)}, (I64)bits);
	}

	void X86LowerPass::idiv(X86Op op, U32 bits) {
		List<MachineOperand> defs = {detail::ph(RAX), detail::ph(RDX)};
		put(op, std::move(defs), {detail::ph(RAX), detail::ph(RCX)}, (I64)bits);
	}

	void X86LowerPass::bitScan(X86Op op, VReg d, VReg s, U32 w) {
		put(op, {detail::vr(d)}, {detail::vr(s)}, (I64)w);
	}

	void X86LowerPass::cmp(VReg a, VReg b) { put(X86Op::Cmp, {}, {detail::vr(a), detail::vr(b)}); }
	void X86LowerPass::cmp(VReg a, Imm b) { put(X86Op::Cmp, {}, {detail::vr(a), detail::im(b)}); }

	void X86LowerPass::setcc(VReg d, U8 cc) { put(X86Op::SetCC, {detail::vr(d)}, {}, (I64)cc); }

	void X86LowerPass::cmov(VReg d, VReg s, U8 cc) {
		put(X86Op::CMov, {detail::vr(d)}, {detail::vr(d), detail::vr(s)}, (I64)cc);
	}

	void X86LowerPass::maskBitsOp(VReg d, U32 bits) {
		MachineInstr& m = put(X86Op::MaskBits, {detail::vr(d)}, {detail::vr(d)}, (I64)bits);
		if(bits > 32)
			m.clobbers = {gpReg(R11)}; // mask built via scratch reg
	}

	void X86LowerPass::signExtBitsOp(VReg d, U32 bits) {
		put(X86Op::SignExtBits, {detail::vr(d)}, {detail::vr(d)}, (I64)bits);
	}

	void X86LowerPass::bswap(VReg d, U32 w) {
		put(X86Op::Bswap, {detail::vr(d)}, {detail::vr(d)}, (I64)w);
	}

	// sse scalar float

	void X86LowerPass::ldf(VReg d, U32 w, VReg base) {
		put(X86Op::FLoad, {detail::vr(d, w)}, {detail::vr(base)});
	}

	void X86LowerPass::ldf(VReg d, U32 w, const String& s) {
		put(X86Op::FLoad, {detail::vr(d, w)}, {MachineOperand::symbol(s)});
	}

	void X86LowerPass::ldf(VReg d, U32 w, const AddrParts& a) {
		put(X86Op::FLoad, {detail::vr(d, w)}, addrUses(a), a.disp, sibBits(0, a));
	}

	void X86LowerPass::stf(const AddrParts& a, VReg s, U32 w) {
		List<MachineOperand> uses = {addrBase(a), detail::vr(s, w)};
		if(a.hasIndex)
			uses.push_back(detail::vr(a.index));
		put(X86Op::FStore, {}, std::move(uses), a.disp, sibBits(0, a));
	}

	void X86LowerPass::farith(X86Op op, VReg d, VReg a, VReg b, U32 w, I64 desc) {
		movaps(d, a, w);
		put(op, {detail::vr(d, w)}, {detail::vr(d, w), detail::vr(b, w)}, desc);
	}

	void X86LowerPass::fneg(VReg d, VReg s, U32 w) {
		// the encoder builds 0-x through the top volatile xmm
		put(X86Op::FNeg, {detail::vr(d, w)}, {detail::vr(s, w)}, (I64)w).clobbers = {
				xmmReg(conv->sseVolatileCount - 1)};
	}

	void X86LowerPass::fsqrt(VReg d, VReg s, U32 w) {
		put(X86Op::FSqrt, {detail::vr(d, w)}, {detail::vr(s, w)}, (I64)w);
	}

	void X86LowerPass::fabs_(VReg d, VReg s, U32 w) {
		put(X86Op::FAbs, {detail::vr(d, w)}, {detail::vr(s, w)}, (I64)w);
	}

	void X86LowerPass::ucomis(VReg d, VReg a, VReg b, U32 w, U8 cc, B32 swap) {
		List<MachineOperand> uses = {detail::vr(a, w), detail::vr(b, w)};
		put(X86Op::FCmp, {detail::vr(d)}, std::move(uses), (I64)cc, swap ? 1 : 0);
	}

	void X86LowerPass::ucomisFlags(VReg a, VReg b, U32 w, B32 swap) {
		put(X86Op::FCmpFlags, {}, {detail::vr(a, w), detail::vr(b, w)}, 0, swap ? 1 : 0);
	}

	void X86LowerPass::cvtf(VReg d, U32 dw, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide) {
		put(X86Op::Cvt, {detail::vr(d, dw)}, {detail::vr(s, sw)}, cvtDesc(pfx, opc, wide));
	}

	void X86LowerPass::cvti(VReg d, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide) {
		put(X86Op::Cvt, {detail::vr(d)}, {detail::vr(s, sw)}, cvtDesc(pfx, opc, wide)).regClass =
				detail::kGp;
	}

	// sse packed vector

	void X86LowerPass::varith(VReg d, VReg a, VReg b, U8 pfx, U8 opc, B32 esc38) {
		I64 desc = ((I64)(esc38 ? 1 : 0) << 16) | ((I64)pfx << 8) | opc;
		farith(X86Op::VArith, d, a, b, 16, desc);
	}

	void X86LowerPass::vsplat(VReg d, VReg s, U32 esz, B32 isInt) {
		List<MachineOperand> uses = {detail::vr(s, isInt ? 8 : esz)};
		put(X86Op::VSplat, {detail::vr(d, 16)}, std::move(uses), (I64)esz, isInt ? 1 : 0);
	}

	void X86LowerPass::vextract(VReg d, VReg s, U32 lane, U32 esz, B32 isInt) {
		I64 desc = ((I64)esz << 1) | (isInt ? 1 : 0);
		MachineInstr& m = put(
				X86Op::VExtract, {detail::vr(d, isInt ? 8 : esz)}, {detail::vr(s, 16)}, (I64)lane, desc);
		m.regClass = isInt ? detail::kGp : detail::kFp;
	}

	void X86LowerPass::vpack(X86Op op, VReg d, List<MachineOperand> lanes, U32 esz, B32 isInt) {
		put(op, {detail::vr(d, 16)}, std::move(lanes), (I64)esz, isInt ? 1 : 0);
	}

	void X86LowerPass::vshuf(VReg d, VReg s, U8 sel) {
		put(X86Op::VShuf, {detail::vr(d, 16)}, {detail::vr(s, 16)}, (I64)sel);
	}

	// x87

	void X86LowerPass::fld(Slot d, VReg addr) {
		put(X86Op::X87LoadMem, {detail::sl(d)}, {detail::vr(addr)}, detail::kX87MemBits);
	}

	void X86LowerPass::fldPop(Slot s) { put(X86Op::X87LoadMem, {}, {detail::sl(s)}, -1); }

	void X86LowerPass::fstp(VReg addr, Slot s) {
		put(X86Op::X87StoreMem, {}, {detail::vr(addr), detail::sl(s)}, detail::kX87MemBits);
	}

	void X86LowerPass::fstp(Slot d) { put(X86Op::X87StoreMem, {detail::sl(d)}, {}, -1); }

	void X86LowerPass::fstpDiscard() { put(X86Op::X87StoreMem, {}, {}, -2); }

	void X86LowerPass::fldImm(Slot d, U64 bits) {
		put(X86Op::X87LoadImmD, {detail::sl(d)}, {MachineOperand::immVal((I64)bits)});
	}

	void X86LowerPass::fild(Slot d, VReg s) {
		put(X86Op::X87FromInt, {detail::sl(d)}, {detail::vr(s)});
	}

	void X86LowerPass::fistp(VReg d, Slot s) {
		put(X86Op::X87ToInt, {detail::vr(d)}, {detail::sl(s)});
	}

	void X86LowerPass::fldSse(Slot d, VReg s, U32 w) {
		put(X86Op::X87FromSse, {detail::sl(d)}, {detail::vr(s, w)}, (I64)w);
	}

	void X86LowerPass::fldSlot(Slot d, Slot s) {
		put(X86Op::X87FromSse, {detail::sl(d)}, {detail::sl(s)}, detail::kX87MemBits);
	}

	void X86LowerPass::fstpSse(VReg d, U32 w, Slot s) {
		put(X86Op::X87ToSse, {detail::vr(d, w)}, {detail::sl(s)}, (I64)w);
	}

	void X86LowerPass::x87Arith(X86Op op, Slot d, Slot a, Slot b) {
		put(op, {detail::sl(d)}, {detail::sl(a), detail::sl(b)});
	}

	void X86LowerPass::fchs(Slot d, Slot s) { put(X86Op::X87Neg, {detail::sl(d)}, {detail::sl(s)}); }

	void X86LowerPass::fucomi(VReg d, Slot a, Slot b, U8 cc, B32 swap) {
		put(X86Op::X87Cmp, {detail::vr(d)}, {detail::sl(a), detail::sl(b)}, (I64)cc, swap ? 1 : 0);
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
				detail::vr(pred), MachineOperand::blockRef(thenB), MachineOperand::blockRef(elseB)};
		put(X86Op::Br, {}, std::move(uses));
	}

	void X86LowerPass::switchJump(VReg sel, const List<I32>& targets) {
		List<MachineOperand> uses = {detail::vr(sel)};
		for(I32 t : targets)
			uses.push_back(MachineOperand::blockRef(t));
		put(X86Op::SwitchJump, {}, std::move(uses)).clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::vaStart(VReg ptr, U32 namedGp, U32 namedFp) {
		MachineInstr& m = put(X86Op::VaStart, {}, {detail::vr(ptr)}, (I64)namedGp, (I64)namedFp);
		m.clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::vaArg(MachineOperand def, VReg ptr, VaArgKind kind, I64 desc, U32 cls) {
		MachineInstr& m = put(X86Op::VaArg, {std::move(def)}, {detail::vr(ptr)}, (I64)kind, desc);
		m.regClass = cls;
		m.clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::ud2() { put(X86Op::Ud2, {}, {}); }

	void X86LowerPass::prefetch(VReg addr, U8 hint) {
		put(X86Op::Prefetch, {}, {detail::vr(addr)}, 0, (I64)hint);
	}
} // namespace rat
