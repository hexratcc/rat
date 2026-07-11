#include "Pass/Emit/X86Emitter.h"

#include "CodeGen/MachineFunction.h"
#include "CodeGen/MachineModule.h"
#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Opcode.h"
#include "IR/Type.h"
#include "Target/Target.h"
#include "Target/X86Asm.h"
#include "Target/X86Elf.h"

namespace rat {
	PhysReg X86LowerPass::gpReg(Reg r) { return X86Target::kGpBase + (PhysReg)r; }
	U32 X86LowerPass::opWidth(const Type* t) {
		if(!t)
			return 8;
		if(t->isPtr())
			return 8;
		if(t->isFloat())
			return t->getFloatWidth() == 32 ? 4 : 8;
		U32 w = t->getIntWidth();
		if(w <= 8)
			return 1;
		if(w <= 16)
			return 2;
		if(w <= 32)
			return 4;
		return 8;
	}

	PhysReg X86LowerPass::xmmReg(U32 n) { return X86Target::kXmmBase + n; }
	B32 X86LowerPass::isFloatTy(const Type* t) { return t && t->isFloat(); }
	B32 X86LowerPass::isX87Ty(const Type* t) {
		return t && t->isFloat() && t->getFloatWidth() == 128;
	}
	B32 X86LowerPass::isSseTy(const Type* t) { return isFloatTy(t) && !isX87Ty(t); }
	U32 X86LowerPass::intBits(const Type* t) { return t && t->isInt() ? t->getIntWidth() : 64; }

	String X86LowerPass::libcName(const String& callee) {
		if(callee.rfind("__builtin_", 0) == 0)
			return callee.substr(10);
		return callee;
	}

	void X86LowerPass::reset(const Function& f, Schedule& s, MachineFunc& o, X86FrameLayout& layout) {
		fn = &f;
		mod = &f.getModule();
		sched = &s;
		out = &o;
		fl = &layout;
		vregOf.clear();
		x87Slot.clear();
		allocOff.clear();
		mb = nullptr;
	}

	I32 X86LowerPass::reserve(U32 bytes) {
		out->frameBytes += bytes;
		out->frameBytes = (out->frameBytes + 7u) & ~7u;
		return -(I32)out->frameBytes;
	}

	void X86LowerPass::layout() {
		for(const Node* n : *fn) {
			if(const AllocNode* al = dyn_cast<AllocNode>(n)) {
				if(!al->isVariableSized()) {
					U32 sz = al->getAllocType()->byteSize(mod->pointerBytes());
					if(sz == 0)
						sz = 8;
					sz = (sz + 7u) & ~7u;
					allocOff[n] = reserve(sz);
				}
			}
		}
		fl->ldScratch = reserve(16);
		fl->variadic = fn->isVariadic();
		if(fl->variadic)
			layoutVariadic();
	}

	void X86LowerPass::layoutVariadic() {
		U32 intIdx = 0, xmmIdx = 0;
		I32 stackBytes = 0;
		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			Type* t = fn->getParamType(i);
			if(isX87Ty(t))
				stackBytes += 16;
			else if(isSseTy(t) && xmmIdx < detail::kMaxXmmArgs)
				++xmmIdx;
			else if(!isFloatTy(t) && intIdx < detail::kMaxIntArgs)
				++intIdx;
			else
				stackBytes += 8;
		}
		fl->namedGp = intIdx;
		fl->namedFp = xmmIdx;
		fl->overflowOff = 16 + stackBytes;
		fl->saveArea = reserve(detail::kRegSaveBytes);
	}

	U32 X86LowerPass::classOf(const Type* t) const {
		if(isX87Ty(t))
			return detail::kX87;
		if(isFloatTy(t))
			return detail::kFp;
		return detail::kGp;
	}

	VReg X86LowerPass::fresh(U32 cls) { return out->newVReg(cls); }

	I32 X86LowerPass::x87SlotOf(const Node* n) {
		auto it = x87Slot.find(n);
		if(it != x87Slot.end())
			return it->second;
		I32 s = reserve(16);
		x87Slot[n] = s;
		return s;
	}

	VReg X86LowerPass::vregFor(const Node* n) {
		auto it = vregOf.find(n);
		if(it != vregOf.end())
			return it->second;
		VReg v = fresh(classOf(n->getType()));
		vregOf[n] = v;
		return v;
	}

	void X86LowerPass::emit(MachineInstr in) { mb->insts.push_back(std::move(in)); }

	MachineInstr& X86LowerPass::inst(
			X86Op op, U32 cls, List<MachineOperand> defs, List<MachineOperand> uses, I64 imm, I64 imm2) {
		MachineInstr m;
		m.op = (MachineOpcode)op;
		m.regClass = cls;
		m.defs = std::move(defs);
		m.uses = std::move(uses);
		m.imm = imm;
		m.imm2 = imm2;
		mb->insts.push_back(std::move(m));
		return mb->insts.back();
	}

	void X86LowerPass::copy(MachineOperand dst, MachineOperand src, U32 cls) {
		inst(X86Op::Copy, cls, {dst}, {src});
	}

	MachineInstr& X86LowerPass::def1(X86Op op, VReg dst, U32 cls, List<MachineOperand> uses) {
		return inst(op, cls, {MachineOperand::vr(dst)}, std::move(uses));
	}

	VReg X86LowerPass::gpValue(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			U64 v = (U64)c->getValue();
			if(n->getType() && n->getType()->isInt())
				v = (U64)signExtend((I64)c->getValue(), opWidth(n->getType()) * 8);
			VReg d = fresh(detail::kGp);
			def1(X86Op::LoadImm, d, detail::kGp, {MachineOperand::immVal((I64)v)});
			return d;
		}
		if(GlobalNode* g = dyn_cast<GlobalNode>(n)) {
			VReg d = fresh(detail::kGp);
			def1(X86Op::LoadSym, d, detail::kGp, {MachineOperand::symbol(g->getSymbol())});
			return d;
		}
		if(AllocNode* al = dyn_cast<AllocNode>(n)) {
			auto it = allocOff.find(al);
			if(it == allocOff.end())
				return vregFor(al); // variable-sized: already materialized
			VReg d = fresh(detail::kGp);
			inst(X86Op::FrameAddr, detail::kGp, {MachineOperand::vr(d)}, {}, it->second);
			return d;
		}
		return vregFor(n);
	}

	VReg X86LowerPass::sseValue(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			U32 w = opWidth(n->getType());
			VReg d = fresh(detail::kFp);
			inst(X86Op::FLoad,
					 detail::kFp,
					 {MachineOperand::vr(d, w)},
					 {MachineOperand::immVal((I64)(U64)c->getValue(), w)});
			return d;
		}
		return vregFor(n);
	}

	I32 X86LowerPass::x87Value(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			I32 s = x87SlotOf(n);
			inst(X86Op::X87LoadImmD,
					 detail::kX87,
					 {MachineOperand::frameSlot(s)},
					 {MachineOperand::immVal((I64)(U64)c->getValue())});
			return s;
		}
		return x87SlotOf(n);
	}

	void X86LowerPass::storeIntTo(VReg addr, VReg src, U32 w) {
		inst(X86Op::Store, detail::kGp, {}, {MachineOperand::vr(addr), MachineOperand::vr(src, w)});
	}

	void X86LowerPass::emitStore(StoreNode* s) {
		Node* val = s->getValue();
		U32 w = opWidth(val->getType());
		VReg addr = gpValue(s->getPointer());
		if(isX87Ty(val->getType())) {
			I32 slot = x87Value(val);
			inst(X86Op::X87StoreMem,
					 detail::kX87,
					 {},
					 {MachineOperand::vr(addr), MachineOperand::frameSlot(slot)},
					 detail::kX87MemBits);
		} else if(isSseTy(val->getType())) {
			VReg v = sseValue(val);
			inst(X86Op::FStore, detail::kFp, {}, {MachineOperand::vr(addr), MachineOperand::vr(v, w)});
		} else {
			VReg v = gpValue(val);
			storeIntTo(addr, v, w);
		}
	}

	void X86LowerPass::emitLoad(LoadNode* l) {
		U32 w = opWidth(l->getType());
		VReg addr = gpValue(l->getPointer());
		if(isX87Ty(l->getType())) {
			inst(X86Op::X87LoadMem,
					 detail::kX87,
					 {MachineOperand::frameSlot(x87SlotOf(l))},
					 {MachineOperand::vr(addr)},
					 detail::kX87MemBits);
		} else if(isSseTy(l->getType())) {
			inst(X86Op::FLoad,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(l), w)},
					 {MachineOperand::vr(addr)});
		} else {
			B32 sign = l->getType() && l->getType()->isInt();
			inst(X86Op::Load,
					 detail::kGp,
					 {MachineOperand::vr(vregFor(l), w)},
					 {MachineOperand::vr(addr)},
					 0,
					 sign ? 1 : 0);
		}
	}

	void X86LowerPass::emitAlloc(AllocNode* al) {
		if(!al->isVariableSized())
			return;
		VReg sz = gpValue(al->getSizeOperand());
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
		VReg rhs = gpValue(n->getRHS());
		VReg d = vregFor(n);
		U32 bits = intBits(n->getType());
		copy(MachineOperand::vr(d), MachineOperand::vr(lhs), detail::kGp);
		if(op == X86Op::LShr)
			maskBits(d, bits);
		copy(MachineOperand::fixed(gpReg(RCX)), MachineOperand::vr(rhs), detail::kGp);
		inst(op,
				 detail::kGp,
				 {MachineOperand::vr(d)},
				 {MachineOperand::vr(d), MachineOperand::fixed(gpReg(RCX))});
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
		VReg lhs = gpValue(n->getLHS());
		VReg rhs = gpValue(n->getRHS());
		twoAddr(mop, vregFor(n), lhs, rhs);
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
		VReg rhs = gpValue(n->getRHS());
		VReg d = vregFor(n);
		inst(X86Op::Cmp, detail::kGp, {}, {MachineOperand::vr(lhs), MachineOperand::vr(rhs)});
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
		case Opcode::Trunc: {
			VReg s = gpValue(src);
			VReg d = vregFor(n);
			copy(MachineOperand::vr(d), MachineOperand::vr(s), detail::kGp);
			signExtBits(d, intBits(n->getType()));
			return;
		}
		case Opcode::SExt: {
			VReg s = gpValue(src);
			VReg d = vregFor(n);
			copy(MachineOperand::vr(d), MachineOperand::vr(s), detail::kGp);
			return;
		}
		case Opcode::ZExt: {
			VReg s = gpValue(src);
			VReg d = vregFor(n);
			copy(MachineOperand::vr(d), MachineOperand::vr(s), detail::kGp);
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
				inst(X86Op::X87FromSse,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(n))},
						 {MachineOperand::frameSlot(s)},
						 detail::kX87MemBits);
				return;
			}
			U32 sw = opWidth(src->getType());
			VReg s = sseValue(src);
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
			inst(X86Op::X87FromInt,
					 detail::kX87,
					 {MachineOperand::frameSlot(x87SlotOf(n))},
					 {MachineOperand::vr(s)});
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			I32 s = x87Value(src);
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

	List<PhysReg> X86LowerPass::callerSavedClobbers() const {
		List<PhysReg> cl;
		for(Reg r : detail::kIntArgRegs)
			cl.push_back(gpReg(r));
		cl.push_back(gpReg(RAX));
		cl.push_back(gpReg(R10));
		cl.push_back(gpReg(R11));
		for(U32 i = 0; i < detail::kMaxXmmArgs; ++i)
			cl.push_back(xmmReg(i));
		return cl;
	}

	void X86LowerPass::emitCall(CallNode* c) {
		if(!c->isIndirect()) {
			const String& callee = c->getCallee();
			if(callee == "__builtin_va_start") {
				emitVaStart(c);
				return;
			}
			if(callee == "__builtin_va_end")
				return;
			if(callee == "__builtin_va_arg") {
				emitVaArg(c);
				return;
			}
		}
		enum ArgClass { Int, Sse, X87 };
		struct ArgLoc {
			Node* node;
			ArgClass cls;
			I32 reg;
		};
		U32 nargs = c->getArgCount();
		U32 intUsed = 0, xmmUsed = 0, stackBytes = 0;
		List<ArgLoc> args;
		args.reserve(nargs);
		auto classify = [&](Node* arg, ArgClass cls, U32& used, U32 max) {
			if(used < max)
				args.push_back({arg, cls, (I32)used++});
			else {
				args.push_back({arg, cls, -1});
				stackBytes += 8;
			}
		};
		for(U32 i = 0; i < nargs; ++i) {
			Node* arg = c->getArg(i);
			if(isX87Ty(arg->getType())) {
				args.push_back({arg, X87, -1});
				stackBytes += 16;
			} else if(isSseTy(arg->getType()))
				classify(arg, Sse, xmmUsed, detail::kMaxXmmArgs);
			else
				classify(arg, Int, intUsed, detail::kMaxIntArgs);
		}

		MachineInstr call;
		call.op = (MachineOpcode)X86Op::Call;
		call.isCall = true;
		call.clobbers = callerSavedClobbers();

		if(c->isIndirect()) {
			VReg t = gpValue(c->getTarget());
			copy(MachineOperand::fixed(gpReg(R11)), MachineOperand::vr(t), detail::kGp);
		}

		{
			VReg al = fresh(detail::kGp);
			def1(X86Op::LoadImm, al, detail::kGp, {MachineOperand::immVal((I64)xmmUsed)});
			copy(MachineOperand::fixed(gpReg(RAX)), MachineOperand::vr(al), detail::kGp);
		}
		for(const ArgLoc& al : args) {
			if(al.reg < 0)
				continue;
			if(al.cls == Sse) {
				VReg v = sseValue(al.node);
				U32 w = opWidth(al.node->getType());
				copy(MachineOperand::fixed(xmmReg((U32)al.reg), w), MachineOperand::vr(v, w), detail::kFp);
				call.uses.push_back(MachineOperand::fixed(xmmReg((U32)al.reg), w));
			} else {
				VReg v = gpValue(al.node);
				copy(MachineOperand::fixed(gpReg(detail::kIntArgRegs[al.reg])),
						 MachineOperand::vr(v),
						 detail::kGp);
				call.uses.push_back(MachineOperand::fixed(gpReg(detail::kIntArgRegs[al.reg])));
			}
		}
		call.uses.push_back(MachineOperand::fixed(gpReg(RAX)));

		if(c->isIndirect()) {
			call.uses.push_back(MachineOperand::fixed(gpReg(R11)));
			call.imm2 = 1; // indirect
		} else {
			call.uses.push_back(MachineOperand::symbol(libcName(c->getCallee())));
		}

		for(const ArgLoc& al : args) {
			if(al.reg >= 0)
				continue;
			if(al.cls == X87) {
				I32 s = x87Value(al.node);
				call.uses.push_back(MachineOperand::frameSlot(s, 16));
			} else if(al.cls == Sse) {
				VReg v = sseValue(al.node);
				call.uses.push_back(MachineOperand::vr(v, opWidth(al.node->getType())));
			} else {
				VReg v = gpValue(al.node);
				call.uses.push_back(MachineOperand::vr(v, 8));
			}
		}
		call.imm = (I64)stackBytes;
		emit(call);

		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt =
				c->returnsValue() ? c->getType()->getTupleElement(CallNode::valueProjIndex()) : nullptr;
		if(rt && isX87Ty(rt) && !vp) {
			inst(X86Op::X87StoreMem, detail::kX87, {}, {}, -2); // pop the unused st(0)
			return;
		}
		if(!vp || !rt)
			return;
		if(isX87Ty(rt)) {
			inst(X86Op::X87StoreMem, detail::kX87, {MachineOperand::frameSlot(x87SlotOf(vp))}, {}, -1);
			return;
		}
		VReg d = vregFor(vp);
		if(isSseTy(rt)) {
			U32 w = opWidth(rt);
			copy(MachineOperand::vr(d, w), MachineOperand::fixed(xmmReg(0), w), detail::kFp);
		} else {
			copy(MachineOperand::vr(d), MachineOperand::fixed(gpReg(RAX)), detail::kGp);
			if(rt->isInt())
				signExtBits(d, intBits(rt));
		}
	}

	void X86LowerPass::emitPrologue() {
		StartNode* st = fn->getStart();
		U32 intIdx = 0, xmmIdx = 0;
		I32 stackOff = 16;
		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			ProjNode* p = st->projection(StartNode::paramProjIndex(i));
			Type* t = fn->getParamType(i);
			if(isX87Ty(t)) {
				if(p) {
					VReg addr = fresh(detail::kGp);
					inst(X86Op::FrameAddr, detail::kGp, {MachineOperand::vr(addr)}, {}, (I64)stackOff);
					inst(X86Op::X87LoadMem,
							 detail::kX87,
							 {MachineOperand::frameSlot(x87SlotOf(p))},
							 {MachineOperand::vr(addr)},
							 detail::kX87MemBits);
				}
				stackOff += 16;
			} else if(isSseTy(t)) {
				if(xmmIdx < detail::kMaxXmmArgs) {
					if(p) {
						U32 w = opWidth(t);
						copy(MachineOperand::vr(vregFor(p), w),
								 MachineOperand::fixed(xmmReg(xmmIdx), w),
								 detail::kFp);
					}
					++xmmIdx;
				} else {
					loadStackParam(p, t, stackOff);
					stackOff += 8;
				}
			} else {
				if(intIdx < detail::kMaxIntArgs) {
					if(p)
						copy(MachineOperand::vr(vregFor(p)),
								 MachineOperand::fixed(gpReg(detail::kIntArgRegs[intIdx])),
								 detail::kGp);
					++intIdx;
				} else {
					loadStackParam(p, t, stackOff);
					stackOff += 8;
				}
			}
		}
	}

	void X86LowerPass::loadStackParam(ProjNode* p, Type* t, I32 disp) {
		if(!p)
			return;
		VReg addr = fresh(detail::kGp);
		inst(X86Op::FrameAddr, detail::kGp, {MachineOperand::vr(addr)}, {}, (I64)disp);
		U32 w = opWidth(t);
		if(isSseTy(t))
			inst(X86Op::FLoad,
					 detail::kFp,
					 {MachineOperand::vr(vregFor(p), w)},
					 {MachineOperand::vr(addr)});
		else
			inst(X86Op::Load,
					 detail::kGp,
					 {MachineOperand::vr(vregFor(p), w)},
					 {MachineOperand::vr(addr)},
					 0,
					 (t && t->isInt()) ? 1 : 0);
	}

	void X86LowerPass::emitVaStart(CallNode* c) {
		VReg ptr = gpValue(c->getArg(0));
		inst(X86Op::VaStart,
				 detail::kGp,
				 {},
				 {MachineOperand::vr(ptr)},
				 (I64)fl->namedGp,
				 (I64)fl->namedFp);
	}

	void X86LowerPass::emitVaArg(CallNode* c) {
		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt = vp ? vp->getType() : nullptr;
		if(!vp || !rt)
			return;
		VReg ptr = gpValue(c->getArg(0));
		VaArgKind kind = isX87Ty(rt) ? VaArgKind::X87 : (isSseTy(rt) ? VaArgKind::Sse : VaArgKind::Int);
		U32 w = opWidth(rt);
		I64 imm2 = (I64)w;
		if(kind == VaArgKind::Int && rt->isInt())
			imm2 |= (I64)1 << 32; // sign-extend the fetched value
		MachineOperand def = kind == VaArgKind::X87 ? MachineOperand::frameSlot(x87SlotOf(vp))
																								: MachineOperand::vr(vregFor(vp), w);
		inst(X86Op::VaArg,
				 kind == VaArgKind::X87 ? detail::kX87 : classOf(rt),
				 {def},
				 {MachineOperand::vr(ptr)},
				 (I64)kind,
				 imm2);
	}

	void X86LowerPass::emitNode(Node* n) {
		switch(n->getOpcode()) {
		case Opcode::Store:
			emitStore(cast<StoreNode>(n));
			return;
		case Opcode::Load:
			emitLoad(cast<LoadNode>(n));
			return;
		case Opcode::Call:
			emitCall(cast<CallNode>(n));
			return;
		case Opcode::Alloc:
			emitAlloc(cast<AllocNode>(n));
			return;
		default:
			break;
		}
		if(isCompareOpcode(n->getOpcode())) {
			emitCompare(cast<CompareNode>(n));
			return;
		}
		if(isConvertOpcode(n->getOpcode())) {
			emitConvert(cast<ConvertNode>(n));
			return;
		}
		if(isUnaryOpcode(n->getOpcode())) {
			emitUnary(cast<UnaryNode>(n));
			return;
		}
		if(isBinaryOpcode(n->getOpcode())) {
			emitBinary(cast<BinaryNode>(n));
			return;
		}
	}

	void X86LowerPass::emitReturn(ReturnNode* r) {
		MachineInstr m;
		m.op = (MachineOpcode)X86Op::Ret;
		if(r->hasValue()) {
			Node* v = r->getValue();
			if(isX87Ty(v->getType())) {
				I32 s = x87Value(v);
				inst(X86Op::X87LoadMem, detail::kX87, {}, {MachineOperand::frameSlot(s)}, -1);
			} else if(isSseTy(v->getType())) {
				U32 w = opWidth(v->getType());
				VReg s = sseValue(v);
				copy(MachineOperand::fixed(xmmReg(0), w), MachineOperand::vr(s, w), detail::kFp);
				m.uses = {MachineOperand::fixed(xmmReg(0), w)};
			} else {
				VReg s = gpValue(v);
				copy(MachineOperand::fixed(gpReg(RAX)), MachineOperand::vr(s), detail::kGp);
				m.uses = {MachineOperand::fixed(gpReg(RAX))};
			}
		}
		emit(m);
	}

	void X86LowerPass::emitPhiCopies(I32 targetBlock, I32 predIdx) {
		const Schedule::Block& tb = sched->block(targetBlock);
		List<PhiNode*> live;
		List<VReg> tmp;
		for(PhiNode* phi : tb.phis) {
			Node* v = phi->getValue(predIdx);
			if(v == phi)
				continue;
			U32 cls = classOf(phi->getType());
			if(cls == detail::kX87) {
				I32 s = x87Value(v);
				inst(X86Op::X87FromSse,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(phi))},
						 {MachineOperand::frameSlot(s)},
						 detail::kX87MemBits);
				continue;
			}
			VReg t = fresh(cls);
			if(cls == detail::kFp) {
				U32 w = opWidth(phi->getType());
				copy(MachineOperand::vr(t, w), MachineOperand::vr(sseValue(v), w), detail::kFp);
			} else
				copy(MachineOperand::vr(t), MachineOperand::vr(gpValue(v)), detail::kGp);
			live.push_back(phi);
			tmp.push_back(t);
		}
		for(size_t i = 0; i < live.size(); i++) {
			PhiNode* phi = live[i];
			U32 cls = classOf(phi->getType());
			VReg d = vregFor(phi);
			if(cls == detail::kFp) {
				U32 w = opWidth(phi->getType());
				copy(MachineOperand::vr(d, w), MachineOperand::vr(tmp[i], w), detail::kFp);
			} else
				copy(MachineOperand::vr(d), MachineOperand::vr(tmp[i]), detail::kGp);
		}
	}

	void X86LowerPass::emitTerminator(I32 b) {
		const Schedule::Block& blk = sched->block(b);
		switch(blk.term) {
		case Schedule::TermKind::Return:
			emitReturn(cast<ReturnNode>(blk.termNode));
			return;
		case Schedule::TermKind::Branch: {
			IfNode* iff = cast<IfNode>(blk.termNode);
			VReg p = gpValue(iff->getPredicate());
			inst(X86Op::Br,
					 detail::kGp,
					 {},
					 {MachineOperand::vr(p),
						MachineOperand::blockRef(blk.thenB),
						MachineOperand::blockRef(blk.elseB)});
			return;
		}
		case Schedule::TermKind::Goto:
			emitPhiCopies(blk.gotoB, blk.gotoPredIdx);
			inst(X86Op::Jmp, detail::kGp, {}, {MachineOperand::blockRef(blk.gotoB)});
			return;
		}
	}

	void X86LowerPass::lowerFunction() {
		layout();
		const List<I32>& order = sched->rpo();
		out->blocks.assign(sched->numBlocks(), {});
		for(U32 i = 0; i < order.size(); ++i) {
			I32 b = order[i];
			MachineBlock& block = out->blocks[b];
			block.id = b;
			mb = &block;
			if(i == 0)
				emitPrologue();
			for(Node* n : sched->block(b).nodes)
				emitNode(n);
			emitTerminator(b);
		}
		for(U32 i = 0; i < order.size(); ++i) {
			I32 b = order[i];
			MachineBlock& block = out->blocks[b];
			for(I32 s : sched->successors(b)) {
				block.succs.push_back(s);
				out->blocks[s].preds.push_back(b);
			}
		}
	}

	B32 X86LowerPass::run(Module& module, MachineModule& mm, const TargetInfo& target) {
		U32 changed = 0;
		for(const Function* fn : module)
			changed += runOnMachineFunction(*fn, mm.get(fn), target);
		return changed != 0;
	}

	U32 X86LowerPass::runOnMachineFunction(const Function& fn, MachineFunc& mf, const TargetInfo&) {
		Schedule sched(fn);
		X86FrameLayout fl;
		mf.src = &fn;
		reset(fn, sched, mf, fl);
		lowerFunction();
		mf.aux = std::make_unique<X86FrameLayout>(fl); // the layout rides along on mf.aux
		return 1;
	}

	RegAllocHooks x86RegAllocHooks() {
		RegAllocHooks hooks;
		hooks.makeReload = [](PhysReg dst, I32 slot, U32 cls, U32 width) {
			MachineInstr m;
			m.op = (MachineOpcode)(cls == detail::kFp ? X86Op::FLoad : X86Op::Load);
			m.regClass = cls;
			m.defs = {MachineOperand::fixed(dst, width)};
			m.uses = {MachineOperand::frameSlot(slot, width)};
			m.imm = 0;
			m.imm2 = 0;
			return m;
		};
		hooks.makeSpill = [](I32 slot, PhysReg src, U32 cls, U32 width) {
			MachineInstr m;
			m.op = (MachineOpcode)(cls == detail::kFp ? X86Op::FStore : X86Op::Store);
			m.regClass = cls;
			m.uses = {MachineOperand::frameSlot(slot, width), MachineOperand::fixed(src, width)};
			m.imm = 0;
			return m;
		};
		hooks.allocSlot = [](MachineFunc& fn, U32 /*cls*/, U32 width) {
			fn.frameBytes += width < 8 ? 8 : width;
			fn.frameBytes = (fn.frameBytes + 7u) & ~7u;
			return -(I32)fn.frameBytes;
		};
		return hooks;
	}

	Reg X86EncodePass::toGp(PhysReg p) { return (Reg)(p - X86Target::kGpBase); }
	U32 X86EncodePass::toXmm(PhysReg p) { return p - X86Target::kXmmBase; }
	Reg X86EncodePass::gpOf(const MachineOperand& o) { return toGp(o.phys); }
	U32 X86EncodePass::xmmOf(const MachineOperand& o) { return toXmm(o.phys); }
	PhysReg X86EncodePass::gpReg11() { return X86Target::kGpBase + (PhysReg)R11; }

	void X86EncodePass::reset(const MachineFunc& f,
														const X86FrameLayout& layout,
														Asm& asm_,
														List<PhysReg> callee) {
		fn = &f;
		fl = &layout;
		a = &asm_;
		blockOffset.clear();
		fixes.clear();
		frameSize = 0;
		calleeSaved = std::move(callee);
		calleeBase = 0;
	}

	void X86EncodePass::readGp(const MachineOperand& o, Reg r) {
		if(o.kind == MachineOperand::Kind::Imm)
			a->movRegImm64(r, (U64)o.imm);
		else if(o.kind == MachineOperand::Kind::Phys) {
			if(gpOf(o) != r)
				a->movRR(r, gpOf(o));
		} else if(o.kind == MachineOperand::Kind::FrameSlot)
			a->load64(r, RBP, o.slot);
	}

	void X86EncodePass::emitCopy(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		if(in.regClass == detail::kFp) {
			U32 w = d.width;
			if(d.kind == MachineOperand::Kind::Phys && s.kind == MachineOperand::Kind::Phys) {
				if(xmmOf(d) != xmmOf(s))
					a->sseArith(0x10, w, xmmOf(d), xmmOf(s)); // movss/movsd reg,reg
			} else if(d.kind == MachineOperand::Kind::Phys && s.kind == MachineOperand::Kind::FrameSlot)
				a->loadXmm(xmmOf(d), RBP, s.slot, w);
			else if(d.kind == MachineOperand::Kind::FrameSlot && s.kind == MachineOperand::Kind::Phys)
				a->storeXmm(xmmOf(s), RBP, d.slot, w);
			return;
		}
		if(d.kind == MachineOperand::Kind::Phys) {
			readGp(s, gpOf(d));
		} else if(d.kind == MachineOperand::Kind::FrameSlot) {
			if(s.kind == MachineOperand::Kind::Phys)
				a->storeMem(RBP, d.slot, gpOf(s), 8);
			else {
				readGp(s, R11);
				a->storeMem(RBP, d.slot, R11, 8);
			}
		}
	}

	void X86EncodePass::emitLoadImm(const MachineInstr& in) {
		a->movRegImm64(gpOf(in.defs[0]), (U64)in.uses[0].imm);
	}

	void X86EncodePass::emitLoadSym(const MachineInstr& in) {
		a->leaRipSym(gpOf(in.defs[0]), in.uses[0].sym, 0);
	}

	void X86EncodePass::emitFrameAddr(const MachineInstr& in) {
		if(in.imm == -1) {
			Reg d = gpOf(in.defs[0]);
			readGp(in.uses[0], R10);
			a->movRegImm64(R11, ~(U64)15);
			a->addRegImm32(R10, 15);
			a->andRR(R10, R11);
			a->subRR(RSP, R10);
			a->andRR(RSP, R11);
			a->movRR(d, RSP);
			return;
		}
		a->leaMem(gpOf(in.defs[0]), RBP, (I32)in.imm);
	}

	void X86EncodePass::emitLoadFrame(const MachineInstr& in) {
		a->leaMem(gpOf(in.defs[0]), RBP, (I32)in.imm);
	}

	void X86EncodePass::emitLoad(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& addr = in.uses[0];
		U32 w = d.width;
		if(addr.kind == MachineOperand::Kind::FrameSlot) {
			a->load64(gpOf(d), RBP, addr.slot);
			return;
		}
		Reg base = gpOf(addr);
		a->loadExt(gpOf(d), base, (I32)in.imm, w, in.imm2 != 0);
	}

	void X86EncodePass::emitStore(const MachineInstr& in) {
		const MachineOperand& a0 = in.uses[0];
		const MachineOperand& src = in.uses[1];
		if(a0.kind == MachineOperand::Kind::FrameSlot) {
			a->storeMem(RBP, a0.slot, gpOf(src), 8);
			return;
		}
		a->storeMem(gpOf(a0), (I32)in.imm, gpOf(src), src.width);
	}

	void X86EncodePass::emitFLoad(const MachineInstr& in) {
		const MachineOperand& d = in.defs[0];
		const MachineOperand& addr = in.uses[0];
		U32 w = d.width;
		if(addr.kind == MachineOperand::Kind::Imm) {
			a->movRegImm64(R11, (U64)addr.imm);
			a->storeMem(RBP, fl->ldScratch, R11, 8);
			a->loadXmm(xmmOf(d), RBP, fl->ldScratch, w);
			return;
		}
		if(addr.kind == MachineOperand::Kind::FrameSlot) {
			a->loadXmm(xmmOf(d), RBP, addr.slot, w);
			return;
		}
		a->loadXmm(xmmOf(d), gpOf(addr), (I32)in.imm, w);
	}

	void X86EncodePass::emitFStore(const MachineInstr& in) {
		const MachineOperand& a0 = in.uses[0];
		const MachineOperand& src = in.uses[1];
		if(a0.kind == MachineOperand::Kind::FrameSlot) {
			a->storeXmm(xmmOf(src), RBP, a0.slot, src.width);
			return;
		}
		a->storeXmm(xmmOf(src), gpOf(a0), (I32)in.imm, src.width);
	}

	void X86EncodePass::emitAlu(const MachineInstr& in, U8 aluOp) {
		Reg d = gpOf(in.defs[0]);
		Reg s = gpOf(in.uses[1]);
		a->aluRR(aluOp, d, s);
	}

	void X86EncodePass::emitMul(const MachineInstr& in) {
		a->imulRR(gpOf(in.defs[0]), gpOf(in.uses[1]));
	}

	void X86EncodePass::emitNegNot(const MachineInstr& in, B32 neg) {
		Reg d = gpOf(in.defs[0]);
		if(neg)
			a->negReg(d);
		else
			a->notReg(d);
	}

	void X86EncodePass::emitShift(const MachineInstr& in, U8 ext) {
		a->shiftCL(ext, gpOf(in.defs[0]));
	}

	void X86EncodePass::emitDiv(const MachineInstr& in, B32 isSigned) {
		B32 wide = (U32)in.imm > 32;
		if(isSigned) {
			a->cqoW(wide);
			a->idivRegW(RCX, wide);
		} else {
			a->xorSelf(RDX);
			a->divRegW(RCX, wide);
		}
		if(!wide) {
			a->movsxd32(RAX, RAX);
			a->movsxd32(RDX, RDX);
		}
	}

	void X86EncodePass::emitMaskBits(const MachineInstr& in) {
		U32 bits = (U32)in.imm;
		if(bits == 0 || bits >= 64)
			return;
		Reg d = gpOf(in.defs[0]);
		a->movRegImm64(R11, ((U64)1 << bits) - 1);
		a->andRR(d, R11);
	}

	void X86EncodePass::emitSignExtBits(const MachineInstr& in) {
		U32 bits = (U32)in.imm;
		if(bits == 0 || bits >= 64)
			return;
		Reg d = gpOf(in.defs[0]);
		if(bits == 32) {
			a->movsxd32(d, d);
			return;
		}
		U8 sh = (U8)(64 - bits);
		a->shiftImm(4, d, sh); // shl
		a->shiftImm(7, d, sh); // sar
	}

	void X86EncodePass::emitCmp(const MachineInstr& in) {
		a->cmpRR(gpOf(in.uses[0]), gpOf(in.uses[1]));
	}

	void X86EncodePass::emitSetCC(const MachineInstr& in) {
		Reg d = gpOf(in.defs[0]);
		a->setcc((U8)in.imm, d);
		a->movzxByte(d, d);
	}

	void X86EncodePass::emitFArith(const MachineInstr& in, U8 op) {
		U32 w = (U32)in.imm;
		a->sseArith(op, w, xmmOf(in.defs[0]), xmmOf(in.uses[1]));
	}

	void X86EncodePass::emitFNeg(const MachineInstr& in) {
		U32 w = (U32)in.imm;
		U32 d = xmmOf(in.defs[0]);
		U32 s = xmmOf(in.uses[0]);
		a->pxor(15, 15);
		a->sseArith(0x5c, w, 15, s == d ? d : s);
		if(d != 15)
			a->sseArith(0x10, w, d, 15);
	}

	void X86EncodePass::emitFCmp(const MachineInstr& in) {
		U32 w = in.uses[0].width;
		U32 lhs = xmmOf(in.uses[0]);
		U32 rhs = xmmOf(in.uses[1]);
		if(in.imm2)
			a->ucomis(w, rhs, lhs);
		else
			a->ucomis(w, lhs, rhs);
		Reg d = gpOf(in.defs[0]);
		a->setcc((U8)in.imm, d);
		a->movzxByte(d, d);
	}

	void X86EncodePass::emitCvt(const MachineInstr& in) {
		U8 pfx = (U8)((in.imm >> 16) & 0xff);
		U8 opc = (U8)((in.imm >> 8) & 0xff);
		B32 w = (in.imm & 1) != 0;
		const MachineOperand& d = in.defs[0];
		const MachineOperand& s = in.uses[0];
		U32 dst = (in.regClass == detail::kGp) ? (U32)gpOf(d) : xmmOf(d);
		U32 srcReg;
		if(in.regClass == detail::kGp)
			srcReg = xmmOf(s); // FPToSI/UI: xmm -> gp
		else if(X86Target::isXmm(s.phys))
			srcReg = xmmOf(s); // fp -> fp
		else
			srcReg = (U32)gpOf(s); // SIToFP/UIToFP: gp -> xmm
		a->cvtRR(pfx, opc, w, dst, srcReg);
	}

	void X86EncodePass::fldSlot(I32 slot) { a->fldT(RBP, slot); }
	void X86EncodePass::fstpSlot(I32 slot) { a->fstpT(RBP, slot); }

	void X86EncodePass::emitX87LoadMem(const MachineInstr& in) {
		if(in.imm == -1) {
			fldSlot(in.uses[0].slot);
			return;
		}
		Reg base = gpOf(in.uses[0]);
		a->fldT(base, 0);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87StoreMem(const MachineInstr& in) {
		if(in.imm == -1) {
			fstpSlot(in.defs[0].slot);
			return;
		}
		if(in.imm == -2) {
			a->fstpReg0();
			return;
		}
		Reg base = gpOf(in.uses[0]);
		fldSlot(in.uses[1].slot);
		a->fstpT(base, 0);
	}

	void X86EncodePass::emitX87LoadImmD(const MachineInstr& in) {
		a->movRegImm64(R11, (U64)in.uses[0].imm);
		a->storeMem(RBP, fl->ldScratch, R11, 8);
		a->fldL(RBP, fl->ldScratch);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87FromInt(const MachineInstr& in) {
		readGp(in.uses[0], R11);
		a->storeMem(RBP, fl->ldScratch, R11, 8);
		a->fildQ(RBP, fl->ldScratch);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87ToInt(const MachineInstr& in) {
		fldSlot(in.uses[0].slot);
		a->fnstcw(RBP, fl->ldScratch + 8);
		a->loadExt(R10, RBP, fl->ldScratch + 8, 2, false);
		a->movRegImm64(R11, 0x0c00);
		a->orRR(R10, R11);
		a->storeMem(RBP, fl->ldScratch + 10, R10, 2);
		a->fldcw(RBP, fl->ldScratch + 10);
		a->fistpQ(RBP, fl->ldScratch);
		a->fldcw(RBP, fl->ldScratch + 8);
		a->load64(gpOf(in.defs[0]), RBP, fl->ldScratch);
	}

	void X86EncodePass::emitX87FromSse(const MachineInstr& in) {
		if(in.imm == 80) {
			fldSlot(in.uses[0].slot);
			fstpSlot(in.defs[0].slot);
			return;
		}
		U32 sw = (U32)in.imm;
		a->storeXmm(xmmOf(in.uses[0]), RBP, fl->ldScratch, sw);
		if(sw == 4)
			a->fldD(RBP, fl->ldScratch);
		else
			a->fldL(RBP, fl->ldScratch);
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87ToSse(const MachineInstr& in) {
		U32 dw = (U32)in.imm;
		fldSlot(in.uses[0].slot);
		if(dw == 4)
			a->fstpD(RBP, fl->ldScratch);
		else
			a->fstpL(RBP, fl->ldScratch);
		a->loadXmm(xmmOf(in.defs[0]), RBP, fl->ldScratch, dw);
	}

	void X86EncodePass::emitX87Binary(const MachineInstr& in, U32 idx) {
		fldSlot(in.uses[0].slot);
		fldSlot(in.uses[1].slot);
		static void (Asm::* const kArith[])() = {&Asm::faddp, &Asm::fsubp, &Asm::fmulp, &Asm::fdivp};
		(a->*kArith[idx])();
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87Neg(const MachineInstr& in) {
		fldSlot(in.uses[0].slot);
		a->fchs();
		fstpSlot(in.defs[0].slot);
	}

	void X86EncodePass::emitX87Cmp(const MachineInstr& in) {
		B32 swap = in.imm2 != 0;
		if(swap) {
			fldSlot(in.uses[0].slot); // -> st(1)
			fldSlot(in.uses[1].slot); // -> st(0)
		} else {
			fldSlot(in.uses[1].slot); // -> st(1)
			fldSlot(in.uses[0].slot); // -> st(0)
		}
		a->fucomip();
		a->fstpReg0();
		Reg d = gpOf(in.defs[0]);
		a->setcc((U8)in.imm, d);
		a->movzxByte(d, d);
	}

	void X86EncodePass::emitVaStart(const MachineInstr& in) {
		Reg ptr = gpOf(in.uses[0]);
		U32 namedGp = (U32)in.imm, namedFp = (U32)in.imm2;
		if(ptr != R10)
			a->movRR(R10, ptr);
		a->movRegImm64(R11, namedGp * 8);
		a->storeMem(R10, 0, R11, 4);
		a->movRegImm64(R11, detail::kGpSaveBytes + namedFp * detail::kXmmSlotBytes);
		a->storeMem(R10, 4, R11, 4);
		a->leaMem(R11, RBP, fl->overflowOff);
		a->storeMem(R10, 8, R11, 8);
		a->leaMem(R11, RBP, fl->saveArea);
		a->storeMem(R10, 16, R11, 8);
	}

	void X86EncodePass::vaFetchOverflow(I32 step) {
		a->load64(R11, R10, 8);									 // R11 = overflow_arg_area
		a->storeMem(RBP, fl->ldScratch, R11, 8); // stash address
		a->addRegImm32(R11, step);							 // advance
		a->storeMem(R10, 8, R11, 8);						 // write back overflow_arg_area
		a->load64(R11, RBP, fl->ldScratch);			 // R11 = stashed address
	}

	void X86EncodePass::vaFetch(I32 offDisp, U32 limit, I32 regStep) {
		a->loadExt(R11, R10, offDisp, 4, false); // R11 = cur offset
		a->cmpRegImm32(R11, (I32)limit);				 // offset vs limit
		U32 toStack = a->jccRel32(CC_AE);				 // offset >= limit -> overflow path
		a->storeMem(RBP, fl->ldScratch, R11, 8); // stash original offset
		a->addRegImm32(R11, regStep);
		a->storeMem(R10, offDisp, R11, 4);	// write advanced offset
		a->load64(R11, RBP, fl->ldScratch); // R11 = original offset
		a->addRegMem(R11, R10, 16);					// R11 += reg_save_area base
		U32 done = a->jmpRel32();
		a->patchRel32(toStack, a->here());
		vaFetchOverflow(8);
		a->patchRel32(done, a->here());
	}

	void X86EncodePass::emitVaArg(const MachineInstr& in) {
		Reg ptr = gpOf(in.uses[0]);
		if(ptr != R10)
			a->movRR(R10, ptr);
		VaArgKind kind = (VaArgKind)in.imm;
		U32 width = (U32)(in.imm2 & 0xffffffff);
		B32 sign = (in.imm2 >> 32) != 0;
		if(kind == VaArgKind::X87) {
			vaFetchOverflow(16);
			a->fldT(R11, 0);
			fstpSlot(in.defs[0].slot);
			return;
		}
		if(kind == VaArgKind::Sse) {
			vaFetch(4, detail::kRegSaveBytes, (I32)detail::kXmmSlotBytes);
			a->loadXmm(xmmOf(in.defs[0]), R11, 0, width);
			return;
		}
		vaFetch(0, detail::kGpSaveBytes, 8);
		a->loadExt(gpOf(in.defs[0]), R11, 0, width, sign);
	}

	void X86EncodePass::emitCall(const MachineInstr& in) {
		I32 stackBytes = (I32)in.imm;
		B32 indirect = in.imm2 != 0;

		U32 targetIdx = 0;
		for(U32 i = 0; i < in.uses.size(); ++i) {
			const MachineOperand& u = in.uses[i];
			if(!indirect && u.kind == MachineOperand::Kind::Sym)
				targetIdx = i;
			else if(indirect && u.kind == MachineOperand::Kind::Phys && u.phys == gpReg11())
				targetIdx = i;
		}
		U32 firstStack = targetIdx + 1;

		B32 pad = (stackBytes & 15) != 0;
		if(pad)
			a->subRegImm32(RSP, 8);
		for(U32 k = in.uses.size(); k-- > firstStack;) {
			const MachineOperand& u = in.uses[k];
			if(u.kind == MachineOperand::Kind::FrameSlot) {
				if(u.width == 16) {
					a->subRegImm32(RSP, 16);
					fldSlot(u.slot);
					a->fstpT(RSP, 0);
				} else {
					a->load64(R11, RBP, u.slot);
					a->push(R11);
				}
			} else if(X86Target::isXmm(u.phys)) {
				a->subRegImm32(RSP, 8);
				a->storeXmm(xmmOf(u), RSP, 0, u.width);
			} else {
				a->push(gpOf(u));
			}
		}

		if(indirect)
			a->callReg(R11);
		else
			a->callSym(in.uses[targetIdx].sym);

		U32 popBytes = (U32)stackBytes + (pad ? 8 : 0);
		if(popBytes)
			a->addRegImm32(RSP, (I32)popBytes);
	}

	void X86EncodePass::recordFix(U32 dispAt, I32 targetBlock) {
		fixes.push_back({dispAt, targetBlock});
	}

	void X86EncodePass::emitRet(const MachineInstr&) {
		for(U32 i = 0; i < calleeSaved.size(); ++i)
			a->load64(toGp(calleeSaved[i]), RBP, calleeBase - (I32)(8 * (i + 1)));
		a->leave();
		a->ret();
	}

	void X86EncodePass::emitJmp(const MachineInstr& in, I32 fallthrough) {
		I32 target = in.uses[0].block;
		if(target == fallthrough)
			return;
		recordFix(a->jmpRel32(), target);
	}

	void X86EncodePass::emitBr(const MachineInstr& in, I32 fallthrough) {
		Reg p = gpOf(in.uses[0]);
		I32 thenB = in.uses[1].block;
		I32 elseB = in.uses[2].block;
		a->testRR(p, p);
		recordFix(a->jccRel32(CC_NE), thenB);
		if(elseB != fallthrough)
			recordFix(a->jmpRel32(), elseB);
	}

	void X86EncodePass::emitInst(const MachineInstr& in, I32 fallthrough) {
		switch((X86Op)in.op) {
		case X86Op::Copy:
			emitCopy(in);
			return;
		case X86Op::LoadImm:
			emitLoadImm(in);
			return;
		case X86Op::LoadSym:
			emitLoadSym(in);
			return;
		case X86Op::LoadFrame:
			emitLoadFrame(in);
			return;
		case X86Op::FrameAddr:
			emitFrameAddr(in);
			return;
		case X86Op::Load:
			emitLoad(in);
			return;
		case X86Op::Store:
			emitStore(in);
			return;
		case X86Op::Add:
		case X86Op::Sub:
		case X86Op::And:
		case X86Op::Or:
		case X86Op::Xor: {
			static const U8 kAlu[] = {
					detail::kAluAdd, detail::kAluSub, 0, detail::kAluAnd, detail::kAluOr, detail::kAluXor};
			static_assert((U32)X86Op::Xor - (U32)X86Op::Add + 1 == 6, "kAlu must cover Add..Xor");
			emitAlu(in, kAlu[(U32)in.op - (U32)X86Op::Add]);
			return;
		}
		case X86Op::Mul:
			emitMul(in);
			return;
		case X86Op::Neg:
		case X86Op::Not:
			emitNegNot(in, (X86Op)in.op == X86Op::Neg);
			return;
		case X86Op::Shl:
			emitShift(in, 4);
			return;
		case X86Op::AShr:
			emitShift(in, 7);
			return;
		case X86Op::LShr:
			emitShift(in, 5);
			return;
		case X86Op::SDiv:
		case X86Op::SRem:
			emitDiv(in, true);
			return;
		case X86Op::UDiv:
		case X86Op::URem:
			emitDiv(in, false);
			return;
		case X86Op::Cmp:
			emitCmp(in);
			return;
		case X86Op::SetCC:
			emitSetCC(in);
			return;
		case X86Op::Movzx:
			a->movzxByte(gpOf(in.defs[0]), gpOf(in.uses[0]));
			return;
		case X86Op::MaskBits:
			emitMaskBits(in);
			return;
		case X86Op::SignExtBits:
			emitSignExtBits(in);
			return;
		case X86Op::FLoad:
			emitFLoad(in);
			return;
		case X86Op::FStore:
			emitFStore(in);
			return;
		case X86Op::FAdd:
		case X86Op::FSub:
		case X86Op::FMul:
		case X86Op::FDiv:
			emitFArith(in, detail::kSseOp[(U32)in.op - (U32)X86Op::FAdd]);
			return;
		case X86Op::FNeg:
			emitFNeg(in);
			return;
		case X86Op::FCmp:
			emitFCmp(in);
			return;
		case X86Op::Cvt:
			emitCvt(in);
			return;
		case X86Op::X87LoadMem:
			emitX87LoadMem(in);
			return;
		case X86Op::X87StoreMem:
			emitX87StoreMem(in);
			return;
		case X86Op::X87LoadImmD:
			emitX87LoadImmD(in);
			return;
		case X86Op::X87FromInt:
			emitX87FromInt(in);
			return;
		case X86Op::X87ToInt:
			emitX87ToInt(in);
			return;
		case X86Op::X87FromSse:
			emitX87FromSse(in);
			return;
		case X86Op::X87ToSse:
			emitX87ToSse(in);
			return;
		case X86Op::X87Add:
		case X86Op::X87Sub:
		case X86Op::X87Mul:
		case X86Op::X87Div:
			emitX87Binary(in, (U32)in.op - (U32)X86Op::X87Add);
			return;
		case X86Op::X87Neg:
			emitX87Neg(in);
			return;
		case X86Op::X87Cmp:
			emitX87Cmp(in);
			return;
		case X86Op::Call:
			emitCall(in);
			return;
		case X86Op::Ret:
			emitRet(in);
			return;
		case X86Op::Jmp:
			emitJmp(in, fallthrough);
			return;
		case X86Op::Br:
			emitBr(in, fallthrough);
			return;
		case X86Op::VaStart:
			emitVaStart(in);
			return;
		case X86Op::VaArg:
			emitVaArg(in);
			return;
		}
	}

	void X86EncodePass::prologue() {
		a->push(RBP);
		a->movRR(RBP, RSP);
		if(frameSize)
			a->subRegImm32(RSP, (I32)frameSize);
		for(U32 i = 0; i < calleeSaved.size(); ++i)
			a->storeMem(RBP, calleeBase - (I32)(8 * (i + 1)), toGp(calleeSaved[i]), 8);
		if(fl->variadic) {
			for(U32 i = 0; i < detail::kMaxIntArgs; ++i)
				a->storeMem(RBP, fl->saveArea + (I32)(i * 8), detail::kIntArgRegs[i], 8);
			a->testRR(RAX, RAX);
			U32 skip = a->jccRel32(CC_E);
			for(U32 i = 0; i < detail::kMaxXmmArgs; ++i)
				a->storeXmm(
						i, RBP, fl->saveArea + (I32)detail::kGpSaveBytes + (I32)(i * detail::kXmmSlotBytes), 8);
			a->patchRel32(skip, a->here());
		}
	}

	void X86EncodePass::encodeFunction() {
		calleeBase = -(I32)fn->frameBytes;
		frameSize = (fn->frameBytes + 8u * (U32)calleeSaved.size() + 15u) & ~15u;
		blockOffset.assign(fn->blocks.size(), 0);
		prologue();
		for(U32 bi = 0; bi < fn->blocks.size(); ++bi) {
			const MachineBlock& blk = fn->blocks[bi];
			if(blk.id < 0)
				continue;
			blockOffset[blk.id] = a->here();
			I32 fallthrough = (bi + 1 < fn->blocks.size()) ? (I32)fn->blocks[bi + 1].id : -1;
			for(const MachineInstr& in : blk.insts)
				emitInst(in, fallthrough);
		}
		for(const JumpFix& f : fixes)
			a->patchRel32(f.dispAt, blockOffset[f.targetBlock]);
	}

	void X86EncodePass::emitGlobal(ElfObject& elf, const Module& mod, const Global* g) {
		const List<U8>& init = g->getInit();
		U32 size = g->getType()->byteSize(mod.pointerBytes());
		if(size == 0)
			size = (U32)init.size();
		if(size == 0)
			size = 1;

		B32 allZero = g->getRelocs().empty() &&
									std::all_of(init.begin(), init.end(), [](U8 v) { return v == 0; });
		ElfObject::Section sec =
				allZero ? ElfObject::Bss : (g->isConstant() ? ElfObject::Rodata : ElfObject::Data);
		elf.align(sec, 8);

		U32 off;
		if(allZero) {
			off = elf.appendZero(sec, size);
		} else {
			List<U8> img(size, 0);
			std::copy_n(init.begin(), init.size() < size ? init.size() : size, img.begin());
			off = elf.append(sec, img.data(), size);
		}
		elf.defineSymbol(g->getName(), sec, off, true, false);

		for(const Reloc& r : g->getRelocs())
			elf.addReloc(sec, off + r.offset, r.symbol, ElfReloc::Abs64, r.addend);
	}

	B32 X86EncodePass::run(Module& mod, MachineModule& mm, const TargetInfo& /*target*/) {
		ElfObject elf;

		for(const Global* g : mod.globals())
			emitGlobal(elf, mod, g);

		for(const Function* fn : mod) {
			MachineFunc& mf = mm.get(fn);

			const X86FrameLayout& fl = *static_cast<const X86FrameLayout*>(mf.aux.get());

			List<U8> code;				 // emitted machine code bytes
			List<AsmReloc> relocs; // relocations into code
			Asm a(code, relocs);
			reset(mf, fl, a, mf.usedCalleeSaved);
			encodeFunction();

			elf.align(ElfObject::Text, 16);
			U32 off = elf.append(ElfObject::Text, code.data(), (U32)code.size());
			elf.defineSymbol(fn->getName(), ElfObject::Text, off, true, true);
			for(const AsmReloc& r : relocs)
				elf.addReloc(ElfObject::Text, off + r.offset, r.symbol, r.kind, r.addend);
		}

		elf.write(*os);
		return false;
	}

	RegAllocHooks X86Target::regAllocHooks() const { return x86RegAllocHooks(); }
} // namespace rat
