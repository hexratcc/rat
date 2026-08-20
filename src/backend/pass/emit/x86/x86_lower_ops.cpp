#include "pass/emit/x86/x86_lower.h"

#include "codegen/machine_function.h"
#include "codegen/machine_module.h"
#include "codegen/schedule.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "target/object_file.h"
#include "target/target.h"
#include "target/x86/x86_asm.h"

namespace rat {
	// narrow load used only by zext: load zero-extended, later zext is a no-op
	B32 X86LowerPass::zextOnlyLoad(const LoadNode* l) {
		const Type* t = l->getType();
		if(!t || !t->isInt() || intBits(t) >= 64)
			return false;
		if(l->getUsers().empty())
			return false;
		for(Node* u : l->getUsers()) {
			ConvertNode* cv = dyn_cast<ConvertNode>(u);
			if(!cv || cv->getOpcode() != Opcode::ZExt || cv->getOperand() != l)
				return false;
		}
		return true;
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
		auto emitOne = [&](X86Op op, U32 cls, MachineOperand src) {
			List<MachineOperand> uses = {addrBase(a), std::move(src)};
			if(a.hasIndex)
				uses.push_back(MachineOperand::vr(a.index));
			inst(op, cls, {}, std::move(uses), a.disp, sibBits(0, a));
		};
		if(isSseTy(val->getType())) {
			emitOne(X86Op::FStore, detail::kFp, MachineOperand::vr(sseValue(val), w));
		} else {
			if(ConstantNode* c = dyn_cast<ConstantNode>(val)) {
				I64 v = c->getValue();
				if(w < 8 || v == (I64)(I32)v) {
					emitOne(X86Op::Store, detail::kGp, MachineOperand::immVal(v, w));
					return;
				}
			}
			MachineOperand src = MachineOperand::vr(gpValue(val), w);
			// indexed reg store [base+index*scale+disp]=src has 3 gp uses; a vreg base lets all three
			// spill at once but only 2 gp scratch exist, so fold the address into an lea and keep the
			// store at <=2 gp uses
			if(a.hasIndex && !a.frameBase) {
				VReg t = fresh(detail::kGp);
				inst(X86Op::Lea,
						 detail::kGp,
						 {MachineOperand::vr(t)},
						 {addrBase(a), MachineOperand::vr(a.index)},
						 a.disp,
						 (I64)(a.scaleLog2 & 3));
				inst(X86Op::Store, detail::kGp, {}, {MachineOperand::vr(t), std::move(src)});
				return;
			}
			emitOne(X86Op::Store, detail::kGp, std::move(src));
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
		List<MachineOperand> uses = {addrBase(a)};
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
			B32 sign = l->getType() && l->getType()->isInt() && !zextOnlyLoad(l);
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
				 -1) // imm -1 marks a dynamic frame address
				.clobbers = {gpReg(R10), gpReg(R11)};
	}

	void X86LowerPass::twoAddr(X86Op op, VReg d, VReg lhs, VReg rhs) {
		copy(MachineOperand::vr(d), MachineOperand::vr(lhs), detail::kGp);
		inst(
				op, detail::kGp, {MachineOperand::vr(d)}, {MachineOperand::vr(d), MachineOperand::vr(rhs)});
	}

	void X86LowerPass::twoAddrF(X86Op op, VReg d, VReg lhs, VReg rhs, U32 w, I64 imm) {
		copy(MachineOperand::vr(d, w), MachineOperand::vr(lhs, w), detail::kFp);
		inst(op,
				 detail::kFp,
				 {MachineOperand::vr(d, w)},
				 {MachineOperand::vr(d, w), MachineOperand::vr(rhs, w)},
				 imm);
	}

	void X86LowerPass::maskBits(VReg d, U32 bits) {
		if(bits > 0 && bits < 64) {
			MachineInstr& m = inst(X86Op::MaskBits,
														 detail::kGp,
														 {MachineOperand::vr(d)},
														 {MachineOperand::vr(d)},
														 (I64)bits);
			if(bits > 32)
				m.clobbers = {gpReg(R11)}; // mask built via scratch reg
		}
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

	// rotates reach lowering only with a constant count (the fold rule builds
	// them that way)
	void X86LowerPass::emitRotate(BinaryNode* n, B32 left) {
		U32 bits = intBits(n->getType());
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		copy(MachineOperand::vr(d), MachineOperand::vr(lhs), detail::kGp);
		I64 iv = 0;
		if(!immOf(n->getRHS(), iv))
			return; // degenerate; should not happen
		inst(left ? X86Op::Rotl : X86Op::Rotr,
				 detail::kGp,
				 {MachineOperand::vr(d)},
				 {MachineOperand::vr(d), MachineOperand::immVal(iv & (bits - 1))},
				 (I64)bits);
		signExtBits(d, bits); // restore in-register sign-extended convention
	}

	void X86LowerPass::emitBinary(BinaryNode* n) {
		Opcode op = n->getOpcode();
		if(n->getType() && n->getType()->isVec()) {
			emitVecBinary(n);
			return;
		}
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
		case Opcode::Rotl:
		case Opcode::Rotr: {
			emitRotate(n, op == Opcode::Rotl);
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
		if(mop == X86Op::Add) {
			// 3-address lea
			inst(X86Op::Lea,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::vr(lhs), MachineOperand::vr(rhs)},
					 0,
					 0);
			return;
		}
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
		twoAddrF(kFOps[idx], d, lhs, rhs, w, (I64)w);
	}

	void X86LowerPass::needVecScratch() {
		if(fl->vecScratch == 0)
			fl->vecScratch = reserve(16);
	}

	String X86LowerPass::vecPoolSym(const List<U8>& bytes) {
		C8 buf[48];
		U64 h = 1469598103934665603ull;
		for(U8 x : bytes) {
			h ^= x;
			h *= 1099511628211ull;
		}
		std::snprintf(buf, sizeof buf, "__rat_vec_%016llx", (unsigned long long)h);
		String name(buf);
		if(!mod->getGlobal(name)) {
			List<U8> init = bytes;
			Global* g = mod->createGlobal(name, mod->getArray(mod->getInt(8), 16), true, std::move(init));
			g->setLinkage(Global::Linkage::Internal);
		}
		return name;
	}

	void X86LowerPass::emitVecBinary(BinaryNode* n) {
		Type* t = n->getType();
		Type* et = t->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		Opcode op = n->getOpcode();

		U8 pfx = 0, opc = 0;
		B32 esc38 = false;
		if(et->isInt()) {
			pfx = 0x66;
			// clang-format off
			switch(op) {
			case Opcode::Add: opc = esz == 4 ? 0xfe : 0xd4; break; // paddd / paddq
			case Opcode::Sub: opc = esz == 4 ? 0xfa : 0xfb; break; // psubd / psubq
			case Opcode::And: opc = 0xdb; break;									 // pand
			case Opcode::Or: opc = 0xeb; break;										 // por
			case Opcode::Xor: opc = 0xef; break;									 // pxor
			case Opcode::Mul: // pmulld, i32 lanes only
				if(esz != 4)
					return;
				opc = 0x40;
				esc38 = true;
				break;
			default: return;
			}
			// clang-format on
		} else {
			pfx = esz == 8 ? 0x66 : 0; // addpd... vs addps...
																 // clang-format off
			switch(op) {
			case Opcode::FAdd: opc = 0x58; break;
			case Opcode::FSub: opc = 0x5c; break;
			case Opcode::FMul: opc = 0x59; break;
			case Opcode::FDiv: opc = 0x5e; break;
			default: return;
			}
																 // clang-format on
		}

		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		twoAddrF(X86Op::VArith, d, lhs, rhs, 16, ((I64)(esc38 ? 1 : 0) << 16) | ((I64)pfx << 8) | opc);
	}

	void X86LowerPass::emitSplat(SplatNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		B32 isInt = !et->isFloat();
		VReg s = isInt ? gpValue(n->getScalar()) : sseValue(n->getScalar());
		inst(X86Op::VSplat,
				 detail::kFp,
				 {MachineOperand::vr(vregFor(n), 16)},
				 {isInt ? MachineOperand::vr(s) : MachineOperand::vr(s, esz)},
				 (I64)esz,
				 isInt ? 1 : 0);
	}

	void X86LowerPass::emitShuffle(ShuffleNode* n) {
		VReg v = sseValue(n->getVector());
		inst(X86Op::VShuf,
				 detail::kFp,
				 {MachineOperand::vr(vregFor(n), 16)},
				 {MachineOperand::vr(v, 16)},
				 (I64)n->getSelector());
	}

	void X86LowerPass::emitExtract(ExtractNode* n) {
		Type* et = n->getType();
		U32 esz = et->byteSize(ptrBytes);
		B32 isInt = !et->isFloat();
		VReg v = sseValue(n->getVector());
		if(isInt && n->getLane() != 0)
			needVecScratch(); // staged through memory
		inst(X86Op::VExtract,
				 isInt ? detail::kGp : detail::kFp,
				 {isInt ? MachineOperand::vr(vregFor(n)) : MachineOperand::vr(vregFor(n), esz)},
				 {MachineOperand::vr(v, 16)},
				 (I64)n->getLane(),
				 ((I64)esz << 1) | (isInt ? 1 : 0));
	}

	void X86LowerPass::emitPack(PackNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		U32 w = n->getLaneCount();
		B32 isInt = !et->isFloat();

		// all-constant packs come from the constant pool as one movups
		B32 allConst = true;
		for(U32 i = 0; i < w; ++i)
			allConst &= isa<ConstantNode>(n->getLane(i));
		if(allConst) {
			List<U8> bytes(16, 0);
			for(U32 i = 0; i < w; ++i) {
				U64 v = (U64)cast<ConstantNode>(n->getLane(i))->getValue();
				for(U32 b = 0; b < esz; ++b)
					bytes[i * esz + b] = (U8)(v >> (8 * b));
			}
			inst(X86Op::FLoad,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), 16)},
					 {MachineOperand::symbol(vecPoolSym(bytes))});
			return;
		}

		// int lanes with sse4.1 build the vector in-register (VPackReg); float and pre-sse4.1
		// fall back to gathering through the vec scratch slot (VPack)
		B32 useReg = isInt && sse41;
		X86Op op = X86Op::VPack;
		if(useReg)
			op = X86Op::VPackReg;
		else
			needVecScratch();
		List<MachineOperand> uses;
		for(U32 i = 0; i < w; ++i) {
			Node* lane = n->getLane(i);
			if(isInt)
				uses.push_back(MachineOperand::vr(gpValue(lane)));
			else
				uses.push_back(MachineOperand::vr(sseValue(lane), esz));
		}
		inst(op,
				 detail::kFp,
				 {MachineOperand::vr(vregFor(n), 16)},
				 std::move(uses),
				 (I64)esz,
				 isInt ? 1 : 0);
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
			// encoder builds 0-x via the top volatile xmm
			inst(X86Op::FNeg,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), w)},
					 {MachineOperand::vr(s, w)},
					 (I64)w)
					.clobbers = {xmmReg(conv->sseVolatileCount - 1)};
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

	// flag-setting cmp only; the caller emits its own jcc/setcc consumer
	void X86LowerPass::emitIntCmp(CompareNode* n) {
		VReg lhs = gpValue(n->getLHS());
		I64 iv;
		if(immOf(n->getRHS(), iv)) {
			inst(X86Op::Cmp, detail::kGp, {}, {MachineOperand::vr(lhs), MachineOperand::immVal(iv)});
		} else {
			VReg rhs = gpValue(n->getRHS());
			inst(X86Op::Cmp, detail::kGp, {}, {MachineOperand::vr(lhs), MachineOperand::vr(rhs)});
		}
	}

	void X86LowerPass::emitCompare(CompareNode* n) {
		Opcode op = n->getOpcode();
		if(op >= Opcode::FEq && op <= Opcode::FGe) {
			emitFloatCompare(n);
			return;
		}
		emitIntCmp(n);
		VReg d = vregFor(n);
		inst(X86Op::SetCC,
				 detail::kGp,
				 {MachineOperand::vr(d)},
				 {},
				 (I64)detail::kIntCc[(U32)op - (U32)Opcode::Eq]);
	}

	void X86LowerPass::emitFloatCompare(CompareNode* n) {
		U32 fcIdx = (U32)n->getOpcode() - (U32)Opcode::FEq;
		U8 cc = detail::kFpCc[fcIdx];
		I64 swap = detail::kFpSwap[fcIdx] ? 1 : 0;
		VReg d = vregFor(n);
		if(isX87Ty(n->getLHS()->getType())) {
			I32 lhs = x87Value(n->getLHS());
			I32 rhs = x87Value(n->getRHS());
			inst(X86Op::X87Cmp,
					 detail::kGp,
					 {MachineOperand::vr(d)},
					 {MachineOperand::frameSlot(lhs), MachineOperand::frameSlot(rhs)},
					 (I64)cc,
					 swap);
			return;
		}
		U32 w = opWidth(n->getLHS()->getType());
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		inst(X86Op::FCmp,
				 detail::kGp,
				 {MachineOperand::vr(d)},
				 {MachineOperand::vr(lhs, w), MachineOperand::vr(rhs, w)},
				 (I64)cc,
				 swap);
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
			if(op == Opcode::Trunc) {
				signExtBits(d, intBits(n->getType()));
			} else if(op == Opcode::ZExt) {
				// already zero-extended when the source load emitted movzx
				LoadNode* ld = dyn_cast<LoadNode>(src);
				if(!ld || !zextOnlyLoad(ld))
					maskBits(d, intBits(src->getType()));
			}
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
		case Opcode::FPExt: { // f32 -> f64: cvtss2sd
			VReg s = sseValue(src);
			inst(X86Op::Cvt,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(n), 8)},
					 {MachineOperand::vr(s, 4)},
					 cvtDesc(0xf3, 0x5a, false));
			return;
		}
		case Opcode::FPTrunc: { // f64 -> f32: cvtsd2ss
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
				x87Move(x87SlotOf(n), s);
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
