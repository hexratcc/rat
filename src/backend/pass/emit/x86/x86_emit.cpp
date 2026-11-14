// x86 operand constructors, the plumbing function put and the named emitters over it. every
// operand is materialized by the caller, so emission order is exactly call order
#include "pass/emit/x86/x86_lower.h"

#include "codegen/machine_function.h"
#include "target/x86/x86_asm.h"

namespace rat {
	MachineOperand X86LowerPass::vr(VReg v, U32 w) { return MachineOperand::vr(v, w); }
	MachineOperand X86LowerPass::ph(Reg r) { return MachineOperand::fixed(detail::gpPhys(r)); }
	MachineOperand X86LowerPass::xm(Xmm x) {
		return MachineOperand::fixed(detail::xmmPhys(x.n), x.w);
	}
	MachineOperand X86LowerPass::im(Imm i) { return MachineOperand::immVal(i.v, i.w); }
	MachineOperand X86LowerPass::sl(Slot s) { return MachineOperand::frameSlot(s.s, s.w); }
	MachineOperand X86LowerPass::blockRef(I32 b) { return MachineOperand::blockRef(b); }

	using detail::kFp;

	MachineInstr& X86LowerPass::put(X86Op op, OpList defs, Ops uses, I64 imm, I64 imm2) {
		MachineInstr& m = mb->insts.emplace_back();
		m.op = (MachineOpcode)op;
		m.regClass = x86OpInfo(op).cls;
		m.defs.assign(defs.begin(), defs.end());
		m.uses = std::move(uses);
		m.imm = imm;
		m.imm2 = imm2;
		m.loc = curLoc;
		// x87 sequences stage through r10/r11 in the encoder, declare so the
		// allocator can use them elsewhere
		if(op >= X86Op::X87LoadMem && op <= X86Op::X87Cmp)
			m.clobbers = detail::scratchBits();
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

	void X86LowerPass::lea(VReg d, const String& s) {
		put(X86Op::LoadSym, {vr(d)}, {MachineOperand::symbol(s)});
	}

	void X86LowerPass::lea(VReg d, const AddrParts& a) {
		Ops uses = {addrBase(a), vr(a.index)};
		put(X86Op::Lea, {vr(d)}, std::move(uses), a.disp, a.scaleLog2 & 3);
	}

	// integer memory

	X86LowerPass::Ops X86LowerPass::addrUses(const AddrParts& a) {
		Ops uses = {addrBase(a)};
		if(a.hasIndex)
			uses.push_back(vr(a.index));
		return uses;
	}

	void X86LowerPass::ld(VReg d, VReg base) { put(X86Op::Load, {vr(d)}, {vr(base)}); }

	void X86LowerPass::ld(VReg d, Slot s) { put(X86Op::Load, {vr(d)}, {sl(s)}); }

	void X86LowerPass::ld(VReg d, U32 w, const AddrParts& a, B32 sign) {
		put(X86Op::Load, {vr(d, w)}, addrUses(a), a.disp, sibBits(sign, a));
	}

	void X86LowerPass::ld(VReg d, U32 w, VReg base, B32 sign) {
		put(X86Op::Load, {vr(d, w)}, {vr(base)}, 0, sign);
	}

	void X86LowerPass::st(VReg base, MachineOperand src) {
		put(X86Op::Store, {}, {vr(base), std::move(src)});
	}

	void X86LowerPass::st(Slot d, Reg src) { put(X86Op::Store, {}, {sl(d), ph(src)}); }

	void X86LowerPass::st(const AddrParts& a, MachineOperand src) {
		Ops uses = {addrBase(a), std::move(src)};
		if(a.hasIndex)
			uses.push_back(vr(a.index));
		put(X86Op::Store, {}, std::move(uses), a.disp, sibBits(0, a));
	}

	// integer ALU

	void X86LowerPass::alu(X86Op op, VReg d, VReg a, VReg b) { put(op, {vr(d)}, {vr(a), vr(b)}); }

	void X86LowerPass::alu(X86Op op, VReg d, VReg a, Imm b) { put(op, {vr(d)}, {vr(a), im(b)}); }

	void X86LowerPass::shift(X86Op op, VReg d, Imm cnt) { put(op, {vr(d)}, {vr(d), im(cnt)}); }

	void X86LowerPass::shift(X86Op op, VReg d, Reg cl) { put(op, {vr(d)}, {vr(d), ph(cl)}); }

	void X86LowerPass::cmp(VReg a, VReg b) { put(X86Op::Cmp, {}, {vr(a), vr(b)}); }
	void X86LowerPass::cmp(VReg a, Imm b) { put(X86Op::Cmp, {}, {vr(a), im(b)}); }

	// sse scalar float

	void X86LowerPass::ldf(VReg d, U32 w, VReg base) { put(X86Op::FLoad, {vr(d, w)}, {vr(base)}); }

	void X86LowerPass::ldf(VReg d, U32 w, const String& s) {
		put(X86Op::FLoad, {vr(d, w)}, {MachineOperand::symbol(s)});
	}

	void X86LowerPass::ldf(VReg d, U32 w, const AddrParts& a) {
		put(X86Op::FLoad, {vr(d, w)}, addrUses(a), a.disp, sibBits(0, a));
	}

	MachineInstr& X86LowerPass::farith(X86Op op, VReg d, VReg a, VReg b, U32 w, I64 desc) {
		movaps(d, a, w);
		VReg rhs = b;
		if(b == a)
			rhs = d; // x op x reads the copy, so a can die at the movaps
		return put(op, {vr(d, w)}, {vr(d, w), vr(rhs, w)}, desc);
	}

	void X86LowerPass::cvtf(VReg d, U32 dw, VReg s, U32 sw, U8 pfx, U8 opc, B32 wide) {
		put(X86Op::Cvt, {vr(d, dw)}, {vr(s, sw)}, cvtDesc(pfx, opc, wide));
	}

	// sse packed vector

	// the VArith/VUnary imm of the instruction pfx 0f esc opc, a pfx or esc of 0 is left out
	I64 X86LowerPass::vecDesc(U8 pfx, U8 opc, U8 esc) {
		return ((I64)esc << 16) | ((I64)pfx << 8) | opc;
	}

	void X86LowerPass::varith(VReg d, VReg a, VReg b, U8 pfx, U8 opc, B32 esc38) {
		U8 esc = 0;
		if(esc38)
			esc = 0x38;
		farith(X86Op::VArith, d, a, b, 16, vecDesc(pfx, opc, esc));
	}

	// d = a op b with an immediate operand, esc is the 0f38/0f3a escape byte or 0
	void X86LowerPass::varithImm(VReg d, VReg a, VReg b, U8 pfx, U8 opc, U8 esc, U8 imm8) {
		I64 desc = vecDesc(pfx, opc, esc) | detail::kVecImm8;
		farith(X86Op::VArith, d, a, b, 16, desc).imm2 = imm8;
	}

	// d = d op s in place (insertps, movsd, movlhps), with an imm8 unless imm8 < 0
	void X86LowerPass::vmerge(VReg d, VReg s, U8 pfx, U8 opc, U8 esc, I32 imm8) {
		I64 desc = vecDesc(pfx, opc, esc);
		if(imm8 >= 0)
			desc |= detail::kVecImm8;
		put(X86Op::VArith, {vr(d, 16)}, {vr(d, 16), vr(s, 16)}, desc, imm8);
	}

	void X86LowerPass::vunary(VReg d, VReg s, U8 pfx, U8 opc, U8 esc) {
		put(X86Op::VUnary, {vr(d, 16)}, {vr(s, 16)}, vecDesc(pfx, opc, esc));
	}

	// d = mask ? t : f per lane, blendvps/blendvpd read the mask from xmm0
	void X86LowerPass::vblend(VReg d, VReg f, VReg t, VReg mask, U32 esz) {
		U8 opc = 0x14; // blendvps
		if(esz == 8)
			opc = 0x15; // blendvpd
		I64 desc = vecDesc(0x66, opc, 0x38);
		movaps(xmm(0, 16), mask);
		MachineInstr& m = farith(X86Op::VArith, d, f, t, 16, desc);
		m.uses.push_back(xm(xmm(0, 16)));
	}

	// x87

	// copies an x87 slot through st0
	void X86LowerPass::fldSlot(Slot d, Slot s) {
		put(X86Op::X87FromSse, {sl(d)}, {sl(s)}, detail::kX87MemBits);
	}

} // namespace rat
