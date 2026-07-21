#include "Pass/Emit/X86Lower.h"

#include "CodeGen/MachineFunction.h"
#include "CodeGen/MachineModule.h"
#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"
#include "Target/ObjectFile.h"
#include "Target/Target.h"
#include "Target/X86Asm.h"

namespace rat {
	VReg X86LowerPass::storeAddr(const AddrParts& a) {
		if(!a.hasIndex)
			return a.base;
		VReg addr = fresh(detail::kGp);
		inst(X86Op::Lea,
				 detail::kGp,
				 {MachineOperand::vr(addr)},
				 {MachineOperand::vr(a.base), MachineOperand::vr(a.index)},
				 0,
				 (I64)a.scaleLog2);
		return addr;
	}

	void X86LowerPass::emitStore(StoreNode* s) {
		Node* val = s->getValue();
		U32 w = opWidth(val->getType());
		if(isX87Ty(val->getType())) {
			VReg addr = gpValue(s->getPointer()); // x87 mem ops carry width in imm, no disp
			I32 slot = x87Value(val);
			inst(X86Op::X87StoreMem,
					 detail::kX87,
					 {},
					 {MachineOperand::vr(addr), MachineOperand::frameSlot(slot)},
					 detail::kX87MemBits);
			return;
		}
		AddrParts a = matchAddr(s->getPointer());
		VReg addr = storeAddr(a);
		if(isSseTy(val->getType())) {
			VReg v = sseValue(val);
			inst(X86Op::FStore,
					 detail::kFp,
					 {},
					 {MachineOperand::vr(addr), MachineOperand::vr(v, w)},
					 a.disp);
		} else {
			if(ConstantNode* c = dyn_cast<ConstantNode>(val)) {
				I64 v = c->getValue();
				if(w < 8 || v == (I64)(I32)v) {
					inst(X86Op::Store,
							 detail::kGp,
							 {},
							 {MachineOperand::vr(addr), MachineOperand::immVal(v, w)},
							 a.disp);
					return;
				}
			}
			VReg v = gpValue(val);
			inst(X86Op::Store,
					 detail::kGp,
					 {},
					 {MachineOperand::vr(addr), MachineOperand::vr(v, w)},
					 a.disp);
		}
	}

	void X86LowerPass::emitLoad(LoadNode* l) {
		U32 w = opWidth(l->getType());
		if(isX87Ty(l->getType())) {
			VReg addr = gpValue(l->getPointer());
			inst(X86Op::X87LoadMem,
					 detail::kX87,
					 {MachineOperand::frameSlot(x87SlotOf(l))},
					 {MachineOperand::vr(addr)},
					 detail::kX87MemBits);
			return;
		}
		AddrParts a = matchAddr(l->getPointer());
		List<MachineOperand> uses = {MachineOperand::vr(a.base)};
		if(a.hasIndex)
			uses.push_back(MachineOperand::vr(a.index));
		if(isSseTy(l->getType())) {
			inst(X86Op::FLoad,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(l), w)},
					 std::move(uses),
					 a.disp,
					 sibBits(0, a));
		} else {
			B32 sign = l->getType() && l->getType()->isInt();
			inst(X86Op::Load,
					 detail::kGp,
					 {MachineOperand::vr(vregFor(l), w)},
					 std::move(uses),
					 a.disp,
					 sibBits(sign ? 1 : 0, a));
		}
	}

	void X86LowerPass::emitAlloc(AllocNode* al) {
		if(!al->isVariableSized())
			return;
		VReg sz = gpValue(al->getSizeOperand());
		needScratch(); // the dynamic FrameAddr sequence stashes through the scratch slot
		inst(X86Op::FrameAddr,
				 detail::kGp,
				 {MachineOperand::vr(vregFor(al))},
				 {MachineOperand::vr(sz)},
				 -1); // imm -1 marks a dynamic frame address
	}

	void X86LowerPass::twoAddr(X86Op op, VReg d, VReg lhs, VReg rhs) {
		copy(MachineOperand::vr(d), MachineOperand::vr(lhs), detail::kGp);
		inst(
				op, detail::kGp, {MachineOperand::vr(d)}, {MachineOperand::vr(d), MachineOperand::vr(rhs)});
	}

	void X86LowerPass::maskBits(VReg d, U32 bits) {
		if(bits > 0 && bits < 64)
			inst(X86Op::MaskBits,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::vr(d)},
					 (I64)bits);
	}

	void X86LowerPass::signExtBits(VReg d, U32 bits) {
		if(bits > 0 && bits < 64)
			inst(X86Op::SignExtBits,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::vr(d)},
					 (I64)bits);
	}

	void X86LowerPass::emitDivLike(BinaryNode* n, X86Op op) {
		Opcode oc = n->getOpcode();
		B32 wantRem = (oc == Opcode::SRem || oc == Opcode::URem);
		VReg lhs = gpValue(n->getLHS());
		VReg rhs = gpValue(n->getRHS());
		VReg d = vregFor(n);

		U32 bits = intBits(n->getType());

		copy(MachineOperand::fixed(gpReg(R11)), MachineOperand::vr(rhs), detail::kGp);
		copy(MachineOperand::fixed(gpReg(RAX)), MachineOperand::vr(lhs), detail::kGp);
		copy(MachineOperand::fixed(gpReg(RCX)), MachineOperand::fixed(gpReg(R11)), detail::kGp);

		inst(op,
				 detail::kGp,
				 {MachineOperand::fixed(gpReg(RAX)), MachineOperand::fixed(gpReg(RDX))},
				 {MachineOperand::fixed(gpReg(RAX)), MachineOperand::fixed(gpReg(RCX))},
				 (I64)bits);

		copy(MachineOperand::vr(d), MachineOperand::fixed(gpReg(wantRem ? RDX : RAX)), detail::kGp);
	}

	void X86LowerPass::emitShift(BinaryNode* n, X86Op op) {
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		U32 bits = intBits(n->getType());
		copy(MachineOperand::vr(d), MachineOperand::vr(lhs), detail::kGp);
		if(op == X86Op::LShr)
			maskBits(d, bits);
		I64 iv;
		if(immOf(n->getRHS(), iv)) { // constant count: shift-by-imm, no RCX
			inst(op,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::vr(d), MachineOperand::immVal(iv & 63)});
		} else {
			VReg rhs = gpValue(n->getRHS());
			copy(MachineOperand::fixed(gpReg(RCX)), MachineOperand::vr(rhs), detail::kGp);
			inst(op,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::vr(d), MachineOperand::fixed(gpReg(RCX))});
		}
		if(op == X86Op::Shl)
			signExtBits(d, bits);
	}

	void X86LowerPass::emitBinary(BinaryNode* n) {
		Opcode op = n->getOpcode();
		if(op >= Opcode::FAdd && op <= Opcode::FDiv) {
			emitFloatBinary(n);
			return;
		}
		switch(op) {
		case Opcode::SDiv:
		case Opcode::UDiv:
		case Opcode::SRem:
		case Opcode::URem: {
			static const X86Op kDiv[] = {X86Op::SDiv, X86Op::UDiv, X86Op::SRem, X86Op::URem};
			static_assert((U32)Opcode::URem - (U32)Opcode::SDiv + 1 == 4, "kDiv must cover SDiv..URem");
			emitDivLike(n, kDiv[(U32)op - (U32)Opcode::SDiv]);
			return;
		}
		case Opcode::Shl:
		case Opcode::LShr:
		case Opcode::AShr: {
			static const X86Op kShift[] = {X86Op::Shl, X86Op::LShr, X86Op::AShr};
			static_assert((U32)Opcode::AShr - (U32)Opcode::Shl + 1 == 3, "kShift must cover Shl..AShr");
			emitShift(n, kShift[(U32)op - (U32)Opcode::Shl]);
			return;
		}
		default:
			break;
		}
		X86Op mop;
		switch(op) {
			// clang-format off
		case Opcode::Add: mop = X86Op::Add; break;
		case Opcode::Sub: mop = X86Op::Sub; break;
		case Opcode::Mul: mop = X86Op::Mul; break;
		case Opcode::And: mop = X86Op::And; break;
		case Opcode::Or:  mop = X86Op::Or;  break;
		case Opcode::Xor: mop = X86Op::Xor; break;
		// clang-format on
		default:
			return;
		}
		Node* ln = n->getLHS();
		Node* rn = n->getRHS();
		I64 iv;
		if(op != Opcode::Sub && !immOf(rn, iv) && immOf(ln, iv))
			std::swap(ln, rn); // commutative ops: put a lone constant on the RHS
		VReg d = vregFor(n);
		if(immOf(rn, iv)) {
			VReg lhs = gpValue(ln);
			if(mop == X86Op::Mul) { // three-operand imul: no tied copy needed
				inst(X86Op::Mul,
						 detail::kGp,
						 {MachineOperand::vr(d)},
						 {MachineOperand::vr(lhs), MachineOperand::immVal(iv)});
				return;
			}
			copy(MachineOperand::vr(d), MachineOperand::vr(lhs), detail::kGp);
			inst(mop,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::vr(d), MachineOperand::immVal(iv)});
			return;
		}
		VReg lhs = gpValue(ln);
		VReg rhs = gpValue(rn);
		twoAddr(mop, d, lhs, rhs);
	}

	void X86LowerPass::emitFloatBinary(BinaryNode* n) {
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::FAdd; // 0..3
		if(isX87Ty(n->getType())) {
			emitX87Binary(n, idx);
			return;
		}
		U32 w = opWidth(n->getType());
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		static const X86Op kFOps[] = {X86Op::FAdd, X86Op::FSub, X86Op::FMul, X86Op::FDiv};
		copy(MachineOperand::vr(d, w), MachineOperand::vr(lhs, w), detail::kFp);
		inst(kFOps[idx],
				 detail::kFp,
				 {MachineOperand::vr(d, w)},
				 {MachineOperand::vr(d, w), MachineOperand::vr(rhs, w)},
				 (I64)w);
	}

	void X86LowerPass::emitX87Binary(BinaryNode* n, U32 idx) {
		static const X86Op kOps[] = {X86Op::X87Add, X86Op::X87Sub, X86Op::X87Mul, X86Op::X87Div};
		I32 lhs = x87Value(n->getLHS());
		I32 rhs = x87Value(n->getRHS());
		inst(kOps[idx],
				 detail::kX87,
				 {MachineOperand::frameSlot(x87SlotOf(n))},
				 {MachineOperand::frameSlot(lhs), MachineOperand::frameSlot(rhs)});
	}

	void X86LowerPass::emitUnary(UnaryNode* n) {
		if(n->getOpcode() == Opcode::FNeg) {
			if(isX87Ty(n->getType())) {
				I32 s = x87Value(n->getOperand());
				inst(X86Op::X87Neg,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(n))},
						 {MachineOperand::frameSlot(s)});
				return;
			}
			U32 w = opWidth(n->getType());
			VReg s = sseValue(n->getOperand());
			needScratch();
			inst(X86Op::FNeg,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), w)},
					 {MachineOperand::vr(s, w)},
					 (I64)w);
			return;
		}
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		copy(MachineOperand::vr(d), MachineOperand::vr(s), detail::kGp);
		inst(n->getOpcode() == Opcode::Neg ? X86Op::Neg : X86Op::Not,
				 detail::kGp,
				 {MachineOperand::vr(d)},
				 {MachineOperand::vr(d)});
	}

	void X86LowerPass::emitCompare(CompareNode* n) {
		Opcode op = n->getOpcode();
		if(op >= Opcode::FEq && op <= Opcode::FGe) {
			emitFloatCompare(n);
			return;
		}
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		I64 iv;
		if(immOf(n->getRHS(), iv)) {
			inst(X86Op::Cmp, detail::kGp, {}, {MachineOperand::vr(lhs), MachineOperand::immVal(iv)});
		} else {
			VReg rhs = gpValue(n->getRHS());
			inst(X86Op::Cmp, detail::kGp, {}, {MachineOperand::vr(lhs), MachineOperand::vr(rhs)});
		}
		inst(X86Op::SetCC,
				 detail::kGp,
				 {MachineOperand::vr(d)},
				 {},
				 (I64)detail::kIntCc[(U32)op - (U32)Opcode::Eq]);
	}

	void X86LowerPass::emitFloatCompare(CompareNode* n) {
		struct FCmp {
			U8 cc;
			B32 swap;
		};
		static const FCmp kFCmp[] = {
				{CC_E, false},	// FEq
				{CC_NE, false}, // FNe
				{CC_A, true},		// FLt
				{CC_AE, true},	// FLe
				{CC_A, false},	// FGt
				{CC_AE, false}, // FGe
		};
		const FCmp& fc = kFCmp[(U32)n->getOpcode() - (U32)Opcode::FEq];
		VReg d = vregFor(n);
		if(isX87Ty(n->getLHS()->getType())) {
			I32 lhs = x87Value(n->getLHS());
			I32 rhs = x87Value(n->getRHS());
			inst(X86Op::X87Cmp,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::frameSlot(lhs), MachineOperand::frameSlot(rhs)},
					 (I64)fc.cc,
					 fc.swap ? 1 : 0);
			return;
		}
		U32 w = opWidth(n->getLHS()->getType());
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		inst(X86Op::FCmp,
				 detail::kGp,
				 {MachineOperand::vr(d)},
				 {MachineOperand::vr(lhs, w), MachineOperand::vr(rhs, w)},
				 (I64)fc.cc,
				 fc.swap ? 1 : 0);
	}

	I64 X86LowerPass::cvtDesc(U8 pfx, U8 opc, B32 w) {
		return ((I64)pfx << 16) | ((I64)opc << 8) | (w ? 1 : 0);
	}

	void X86LowerPass::emitConvert(ConvertNode* n) {
		Node* src = n->getOperand();
		Opcode op = n->getOpcode();
		if(isX87Ty(n->getType()) || isX87Ty(src->getType())) {
			emitConvertX87(n, src, op);
			return;
		}
		switch(op) {
		case Opcode::Trunc:
		case Opcode::SExt:
		case Opcode::ZExt: {
			// values live sign-extended in 64-bit registers
			VReg s = gpValue(src);
			VReg d = vregFor(n);
			copy(MachineOperand::vr(d), MachineOperand::vr(s), detail::kGp);
			if(op == Opcode::Trunc)
				signExtBits(d, intBits(n->getType()));
			else if(op == Opcode::ZExt)
				maskBits(d, intBits(src->getType()));
			return;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP: {
			U32 w = opWidth(n->getType());
			VReg s = gpValue(src);
			inst(X86Op::Cvt,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), w)},
					 {MachineOperand::vr(s)},
					 cvtDesc(Asm::ssePrefixByte(w), 0x2a, true));
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			U32 w = opWidth(src->getType());
			VReg s = sseValue(src);
			inst(X86Op::Cvt,
					 detail::kGp,
					 {MachineOperand::vr(vregFor(n))},
					 {MachineOperand::vr(s, w)},
					 cvtDesc(Asm::ssePrefixByte(w), 0x2c, true));
			return;
		}
		case Opcode::FPExt: {
			VReg s = sseValue(src);
			inst(X86Op::Cvt,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), 8)},
					 {MachineOperand::vr(s, 4)},
					 cvtDesc(0xf3, 0x5a, false));
			return;
		}
		case Opcode::FPTrunc: {
			VReg s = sseValue(src);
			inst(X86Op::Cvt,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), 4)},
					 {MachineOperand::vr(s, 8)},
					 cvtDesc(0xf2, 0x5a, false));
			return;
		}
		default:
			return;
		}
	}

	void X86LowerPass::emitConvertX87(ConvertNode* n, Node* src, Opcode op) {
		switch(op) {
		case Opcode::FPExt: {
			if(isX87Ty(src->getType())) { // long double -> long double: plain move
				I32 s = x87Value(src);
				needScratch();
				inst(X86Op::X87FromSse,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(n))},
						 {MachineOperand::frameSlot(s)},
						 detail::kX87MemBits);
				return;
			}
			U32 sw = opWidth(src->getType());
			VReg s = sseValue(src);
			needScratch();
			inst(X86Op::X87FromSse,
					 detail::kX87,
					 {MachineOperand::frameSlot(x87SlotOf(n))},
					 {MachineOperand::vr(s, sw)},
					 (I64)sw);
			return;
		}
		case Opcode::FPTrunc: {
			I32 s = x87Value(src);
			U32 dw = opWidth(n->getType());
			needScratch();
			inst(X86Op::X87ToSse,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), dw)},
					 {MachineOperand::frameSlot(s)},
					 (I64)dw);
			return;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP: {
			VReg s = gpValue(src);
			needScratch();
			inst(X86Op::X87FromInt,
					 detail::kX87,
					 {MachineOperand::frameSlot(x87SlotOf(n))},
					 {MachineOperand::vr(s)});
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			I32 s = x87Value(src);
			needScratch();
			inst(X86Op::X87ToInt,
					 detail::kGp,
					 {MachineOperand::vr(vregFor(n))},
					 {MachineOperand::frameSlot(s)});
			return;
		}
		default:
			return;
		}
	}

} // namespace rat
