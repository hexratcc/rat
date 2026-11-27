// the caller materializes every operand, so emission order is call order
#include "pass/emit/x86/x86_lower.h"

#include "codegen/machine_function.h"
#include "target/x86/x86_asm.h"

namespace rat {
	MachineOperand X86LowerPass::vr(VReg v, U32 w) { return MachineOperand::vr(v, w); }
	MachineOperand X86LowerPass::ph(Reg r) { return MachineOperand::fixed(detail::gpPhys(r)); }
	MachineOperand X86LowerPass::xm(U32 n, U32 w) {
		return MachineOperand::fixed(detail::xmmPhys(n), w);
	}
	MachineOperand X86LowerPass::im(I64 v, U32 w) { return MachineOperand::immVal(v, w); }
	MachineOperand X86LowerPass::sl(I32 s, U32 w) { return MachineOperand::frameSlot(s, w); }

	MachineOperand X86LowerPass::addrBase(const AddrParts& a) {
		return a.frameBase ? ph(RBP) : vr(a.base);
	}

	I64 X86LowerPass::sibBits(B32 sign, const AddrParts& a) {
		I64 indexed = a.index != kNoVReg ? detail::kSibIndexed : 0;
		I64 signBit = sign ? detail::kSibSign : 0;
		return signBit | indexed | ((I64)(a.scaleLog2 & 3) << 2);
	}

	MachineInstr& X86LowerPass::put(X86Op op, OpList defs, Ops uses, I64 imm, I64 imm2) {
		MachineInstr& m = mb->insts.emplace_back();
		m.op = (MachineOpcode)op;
		m.defs.assign(defs.begin(), defs.end());
		m.uses = std::move(uses);
		m.imm = imm;
		m.imm2 = imm2;
		m.loc = curLoc;
		// the encoder stages x87 sequences through r10 and r11
		if(op >= X86Op::X87LoadMem && op <= X86Op::X87Cmp)
			m.clobbers = detail::scratchBits();
		return m;
	}

	void X86LowerPass::mov(MachineOperand d, MachineOperand s) { put(X86Op::Copy, {d}, {s}); }

	void X86LowerPass::lea(VReg d, const AddrParts& a) {
		put(X86Op::Lea, {vr(d)}, {addrBase(a), vr(a.index)}, a.disp, sibBits(false, a));
	}

	void X86LowerPass::load(X86Op op, VReg d, U32 w, const AddrParts& a, B32 sign) {
		Ops uses = {addrBase(a)};
		if(a.index != kNoVReg)
			uses.push_back(vr(a.index));
		put(op, {vr(d, w)}, std::move(uses), a.disp, sibBits(sign, a));
	}

	void X86LowerPass::store(X86Op op, const AddrParts& a, MachineOperand src) {
		Ops uses = {addrBase(a), src};
		if(a.index != kNoVReg)
			uses.push_back(vr(a.index));
		put(op, {}, std::move(uses), a.disp, sibBits(false, a));
	}

	void X86LowerPass::alu(X86Op op, VReg d, MachineOperand rhs, U32 w) {
		put(op, {vr(d, w)}, {vr(d, w), rhs});
	}

	void X86LowerPass::cmp(VReg lhs, MachineOperand rhs, U32 w) {
		put(X86Op::Cmp, {}, {vr(lhs, w), rhs});
	}

	// one Sse instr, an imm8 >= 0 follows the modrm
	MachineInstr& X86LowerPass::sse(Enc e, MachineOperand d, Ops uses, I32 imm8) {
		I64 desc = detail::packEnc(e, imm8 >= 0);
		return put(X86Op::Sse, {d}, std::move(uses), desc, std::max(imm8, 0));
	}

	Enc X86LowerPass::scalarEnc(U8 opc, U32 w, B32 rexW) {
		return {Asm::ssePrefixByte(w), 0x0f, opc, rexW};
	}

	MachineInstr& X86LowerPass::farith(Enc e, VReg d, VReg a, VReg b, U32 w, I32 imm8) {
		mov(vr(d, w), vr(a, w));
		VReg rhs = b == a ? d : b; // x op x reads the copy, so a can die at the copy
		return sse(e, vr(d, w), {vr(d, w), vr(rhs, w)}, imm8);
	}

	MachineInstr& X86LowerPass::varith(Enc e, VReg d, VReg a, VReg b, I32 imm8) {
		return farith(e, d, a, b, 16, imm8);
	}

	// d = d op s in place (insertps, movsd, movlhps)
	void X86LowerPass::vmerge(Enc e, VReg d, VReg s, I32 imm8) {
		sse(e, vr(d, 16), {vr(d, 16), vr(s, 16)}, imm8);
	}

	// copies an x87 slot through st0
	void X86LowerPass::fldSlot(I32 d, I32 s) {
		put(X86Op::X87FromSse, {sl(d)}, {sl(s)}, detail::kX87MemBits);
	}
} // namespace rat
