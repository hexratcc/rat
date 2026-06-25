#include "Pass/Emit/X86Emitter.h"

#include "Target/X86Asm.h"
#include "Target/X86Elf.h"

#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "Target/Target.h"

namespace rat {
	B32 X86EmitterPass::isFloatTy(const Type* t) { return t && t->isFloat(); }
	B32 X86EmitterPass::isX87Ty(const Type* t) {
		return t && t->isFloat() && t->getFloatWidth() == 128;
	}

	String X86EmitterPass::libcName(const String& callee) {
		if(callee.rfind("__builtin_", 0) == 0)
			return callee.substr(10);
		return callee;
	}

	U32 X86EmitterPass::intBits(const Type* t) { return t && t->isInt() ? t->getIntWidth() : 64; }

	U32 X86EmitterPass::opWidth(const Type* t) {
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

	X86EmitterPass::FunctionEmitter::FunctionEmitter(const Function& f)
	: fn(f),
		mod(f.getModule()),
		sched(f),
		a(code, relocs) {}

	I32 X86EmitterPass::FunctionEmitter::slotOf(const Node* n) {
		auto it = slot.find(n);
		return it == slot.end() ? 0 : it->second;
	}

	void X86EmitterPass::FunctionEmitter::layout() {
		U32 off = 0;
		auto reserve = [&](const Node* n, U32 size) {
			off += size;
			off = (off + 7) & ~7u;
			slot[n] = -(I32)off;
		};
		for(const Node* n : fn) {
			Opcode op = n->getOpcode();
			if(const AllocNode* al = dyn_cast<AllocNode>(n)) {
				if(!al->isVariableSized()) {
					U32 sz = al->getAllocType()->byteSize(mod.pointerBytes());
					if(sz == 0)
						sz = 8;
					sz = (sz + 7) & ~7u;
					off += sz;
					off = (off + 7) & ~7u;
					allocOff[n] = -(I32)off;
					reserve(n, 8);
					continue;
				}
				reserve(n, 8);
				continue;
			}
			Type* t = n->getType();
			B32 producesValue = false;
			if(op == Opcode::Call) {
				// handled via its value projection
			} else if(op == Opcode::Proj) {
				const ProjNode* p = cast<ProjNode>(n);
				Node* prod = p->getProducer();
				if(prod->getOpcode() == Opcode::Start && p->getIndex() >= 2)
					producesValue = true; // parameter
				else if(prod->getOpcode() == Opcode::Call && p->getIndex() == CallNode::valueProjIndex())
					producesValue = true; // call result
			} else if(t && t->isData()) {
				producesValue = Schedule::isFloating(n) || op == Opcode::Constant || op == Opcode::Phi ||
												op == Opcode::Global;
			}
			if(producesValue && !slot.count(n))
				reserve(n, isX87Ty(t) ? 16 : 8);
		}
		off += 16;
		off = (off + 15) & ~15u;
		ldScratch = -(I32)off;
		variadic = fn.isVariadic();
		if(variadic)
			layoutVariadic(off);
		frameSize = (off + 15) & ~15u;
	}

	void X86EmitterPass::FunctionEmitter::layoutVariadic(U32& off) {
		U32 intIdx = 0, xmmIdx = 0;
		I32 stackBytes = 0;
		for(U32 i = 0; i < fn.getParamCount(); ++i) {
			Type* t = fn.getParamType(i);
			if(isX87Ty(t))
				stackBytes += 16;
			else if(isFloatTy(t) && xmmIdx < kMaxXmmArgs)
				++xmmIdx;
			else if(!isFloatTy(t) && intIdx < kMaxIntArgs)
				++intIdx;
			else
				stackBytes += 8;
		}
		namedGp = intIdx;
		namedFp = xmmIdx;
		overflowOff = 16 + stackBytes;
		off += kRegSaveBytes;
		off = (off + 15) & ~15u;
		saveArea = -(I32)off;
	}

	void X86EmitterPass::FunctionEmitter::loadInt(Node* n, Reg r) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			U64 v = (U64)c->getValue();
			if(n->getType() && n->getType()->isInt())
				v = (U64)signExtend((I64)v, opWidth(n->getType()) * 8);
			a.movRegImm64(r, v);
			return;
		}
		if(GlobalNode* g = dyn_cast<GlobalNode>(n)) {
			a.leaRipSym(r, g->getSymbol(), 0);
			return;
		}
		if(AllocNode* al = dyn_cast<AllocNode>(n)) {
			auto it = allocOff.find(al);
			if(it != allocOff.end())
				a.leaMem(r, RBP, it->second);
			else
				a.load64(r, RBP, slotOf(al));
			return;
		}
		a.loadExt(r, RBP, slotOf(n), opWidth(n->getType()), true);
	}

	void X86EmitterPass::FunctionEmitter::loadFloat(Node* n, U32 x) {
		U32 w = opWidth(n->getType());
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			a.movRegImm64(RAX, (U64)c->getValue());
			I32 s = slotOf(n);
			if(s == 0)
				s = ldScratch;
			a.storeMem(RBP, s, RAX, 8);
			a.loadXmm(x, RBP, s, w);
			return;
		}
		a.loadXmm(x, RBP, slotOf(n), w);
	}

	void X86EmitterPass::FunctionEmitter::storeInt(Node* n, Reg r) {
		a.storeMem(RBP, slotOf(n), r, 8);
	}
	void X86EmitterPass::FunctionEmitter::storeFloat(Node* n, U32 x) {
		a.storeXmm(x, RBP, slotOf(n), opWidth(n->getType()));
	}

	void X86EmitterPass::FunctionEmitter::fldX87(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			a.movRegImm64(RAX, (U64)c->getValue());
			a.storeMem(RBP, ldScratch, RAX, 8);
			a.fldL(RBP, ldScratch); // double -> ST0 (extended)
			return;
		}
		a.fldT(RBP, slotOf(n)); // 80-bit slot -> ST0
	}
	void X86EmitterPass::FunctionEmitter::fstpX87Slot(Node* n) { a.fstpT(RBP, slotOf(n)); }

	void X86EmitterPass::FunctionEmitter::run() {
		layout();
		blockOffset.assign(sched.numBlocks(), 0);

		prologue();

		const List<I32>& order = sched.rpo();
		for(U32 i = 0; i < order.size(); ++i) {
			I32 b = order[i];
			blockOffset[b] = a.here();
			for(Node* n : sched.block(b).nodes)
				emitNode(n);
			emitTerminator(b, i + 1 < order.size() ? order[i + 1] : -1);
		}

		for(const JumpFix& f : fixes)
			a.patchRel32(f.dispAt, blockOffset[f.targetBlock]);
	}

	void X86EmitterPass::FunctionEmitter::prologue() {
		a.push(RBP);
		a.movRR(RBP, RSP);
		if(frameSize)
			a.subRegImm32(RSP, (I32)frameSize);

		if(variadic) {
			for(U32 i = 0; i < kMaxIntArgs; ++i)
				a.storeMem(RBP, saveArea + (I32)(i * 8), kIntArgRegs[i], 8);
			a.testRR(RAX, RAX);
			U32 skip = a.jccRel32(CC_E);
			for(U32 i = 0; i < kMaxXmmArgs; ++i)
				a.storeXmm(i, RBP, saveArea + (I32)kGpSaveBytes + (I32)(i * kXmmSlotBytes), 8);
			a.patchRel32(skip, a.here());
		}
		StartNode* st = fn.getStart();
		U32 intIdx = 0, xmmIdx = 0;
		I32 stackOff = 16;
		for(U32 i = 0; i < fn.getParamCount(); ++i)
			emitParamLoad(st->projection(StartNode::paramProjIndex(i)),
										fn.getParamType(i),
										intIdx,
										xmmIdx,
										stackOff);
	}

	B32 X86EmitterPass::FunctionEmitter::wantsSlot(ProjNode* p) const { return p && slot.count(p); }

	void X86EmitterPass::FunctionEmitter::emitParamLoad(
			ProjNode* p, Type* t, U32& intIdx, U32& xmmIdx, I32& stackOff) {
		auto spillStackArg = [&] {
			a.load64(RAX, RBP, stackOff);
			stackOff += 8;
			if(wantsSlot(p))
				a.storeMem(RBP, slotOf(p), RAX, 8);
		};
		if(isX87Ty(t)) {
			if(wantsSlot(p)) {
				a.fldT(RBP, stackOff);
				a.fstpT(RBP, slotOf(p));
			}
			stackOff += 16;
		} else if(isFloatTy(t)) {
			if(xmmIdx < kMaxXmmArgs) {
				if(wantsSlot(p))
					a.storeXmm(xmmIdx, RBP, slotOf(p), opWidth(t));
				++xmmIdx;
			} else {
				spillStackArg();
			}
		} else {
			if(intIdx < kMaxIntArgs) {
				if(wantsSlot(p))
					a.storeMem(RBP, slotOf(p), kIntArgRegs[intIdx], 8);
				++intIdx;
			} else {
				spillStackArg();
			}
		}
	}

	void X86EmitterPass::FunctionEmitter::emitNode(Node* n) {
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

	void X86EmitterPass::FunctionEmitter::emitAlloc(AllocNode* al) {
		if(!al->isVariableSized())
			return;
		loadInt(al->getSizeOperand(), RAX);
		a.movRegImm64(RCX, ~(U64)15);
		a.addRegImm32(RAX, 15);
		a.andRR(RAX, RCX);
		a.subRR(RSP, RAX);
		a.andRR(RSP, RCX);
		if(slot.count(al))
			a.storeMem(RBP, slotOf(al), RSP, 8);
	}

	void X86EmitterPass::FunctionEmitter::emitStore(StoreNode* s) {
		Node* val = s->getValue();
		U32 w = opWidth(val->getType());
		loadInt(s->getPointer(), R10);
		if(isX87Ty(val->getType())) {
			fldX87(val);
			a.fstpT(R10, 0);
		} else if(isFloatTy(val->getType())) {
			loadFloat(val, 0);
			a.storeXmm(0, R10, 0, w);
		} else {
			loadInt(val, RAX);
			a.storeMem(R10, 0, RAX, w);
		}
	}

	void X86EmitterPass::FunctionEmitter::emitLoad(LoadNode* l) {
		U32 w = opWidth(l->getType());
		loadInt(l->getPointer(), R10);
		if(isX87Ty(l->getType())) {
			a.fldT(R10, 0);
			fstpX87Slot(l);
		} else if(isFloatTy(l->getType())) {
			a.loadXmm(0, R10, 0, w);
			storeFloat(l, 0);
		} else {
			B32 sign = l->getType() && l->getType()->isInt();
			a.loadExt(RAX, R10, 0, w, sign);
			storeInt(l, RAX);
		}
	}

	void X86EmitterPass::FunctionEmitter::emitBinary(BinaryNode* n) {
		Opcode op = n->getOpcode();
		if(op >= Opcode::FAdd && op <= Opcode::FDiv) {
			emitFloatBinary(n);
			return;
		}
		if(op == Opcode::UDiv || op == Opcode::URem) {
			loadInt(n->getLHS(), RAX);
			loadInt(n->getRHS(), RCX);
			maskToBits(intBits(n->getType()), RAX, RCX);
			a.xorSelf(RDX);
			a.divReg(RCX);
			if(op == Opcode::URem)
				a.movRR(RAX, RDX);
			storeInt(n, RAX);
			return;
		}
		if(op == Opcode::LShr) {
			loadInt(n->getLHS(), RAX);
			loadInt(n->getRHS(), RCX);
			maskToBits(intBits(n->getType()), RAX);
			a.shiftCL(5, RAX);
			storeInt(n, RAX);
			return;
		}
		loadInt(n->getLHS(), RAX);
		loadInt(n->getRHS(), RCX);
		switch(op) {
		case Opcode::Add:
			a.addRR(RAX, RCX);
			break;
		case Opcode::Sub:
			a.subRR(RAX, RCX);
			break;
		case Opcode::Mul:
			a.imulRR(RAX, RCX);
			break;
		case Opcode::And:
			a.andRR(RAX, RCX);
			break;
		case Opcode::Or:
			a.orRR(RAX, RCX);
			break;
		case Opcode::Xor:
			a.xorRR(RAX, RCX);
			break;
		case Opcode::Shl:
			a.shiftCL(4, RAX);
			break;
		case Opcode::AShr:
			a.shiftCL(7, RAX);
			break;
		case Opcode::SDiv:
			a.cqo();
			a.idivReg(RCX);
			break;
		case Opcode::SRem:
			a.cqo();
			a.idivReg(RCX);
			a.movRR(RAX, RDX);
			break;
		default:
			break;
		}
		storeInt(n, RAX);
	}

	void X86EmitterPass::FunctionEmitter::emitFloatBinary(BinaryNode* n) {
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::FAdd;
		if(isX87Ty(n->getType())) {
			fldX87(n->getLHS());
			fldX87(n->getRHS());
			switch(idx) {
			case 0:
				a.faddp();
				break;
			case 1:
				a.fsubp();
				break;
			case 2:
				a.fmulp();
				break;
			case 3:
				a.fdivp();
				break;
			}
			fstpX87Slot(n);
			return;
		}
		U32 w = opWidth(n->getType());
		loadFloat(n->getLHS(), 0);
		loadFloat(n->getRHS(), 1);
		a.sseArith(kSseOp[idx], w, 0, 1);
		storeFloat(n, 0);
	}

	void X86EmitterPass::FunctionEmitter::emitUnary(UnaryNode* n) {
		if(n->getOpcode() == Opcode::FNeg) {
			if(isX87Ty(n->getType())) {
				fldX87(n->getOperand());
				a.fchs();
				fstpX87Slot(n);
				return;
			}
			U32 w = opWidth(n->getType());
			loadFloat(n->getOperand(), 0);
			a.pxor(1, 1);
			a.sseArith(0x5c, w, 1, 0);
			storeFloat(n, 1);
			return;
		}
		loadInt(n->getOperand(), RAX);
		if(n->getOpcode() == Opcode::Neg)
			a.negReg(RAX);
		else
			a.notReg(RAX);
		storeInt(n, RAX);
	}

	void X86EmitterPass::FunctionEmitter::maskToBits(U32 bits, Reg r0, Reg r1) {
		if(bits == 0 || bits >= 64)
			return;
		a.movRegImm64(RDX, ((U64)1 << bits) - 1);
		a.andRR(r0, RDX);
		if(r1 != r0)
			a.andRR(r1, RDX);
	}

	void X86EmitterPass::FunctionEmitter::storeBool(Node* n, U8 cc) {
		a.setcc(cc, RAX);
		a.movzxByte(RAX, RAX);
		storeInt(n, RAX);
	}

	void X86EmitterPass::FunctionEmitter::emitCompare(CompareNode* n) {
		Opcode op = n->getOpcode();
		if(op >= Opcode::FEq && op <= Opcode::FGe) {
			emitFloatCompare(n);
			return;
		}
		loadInt(n->getLHS(), RAX);
		loadInt(n->getRHS(), RCX);
		a.cmpRR(RAX, RCX);
		storeBool(n, kIntCc[(U32)op - (U32)Opcode::Eq]);
	}

	void X86EmitterPass::FunctionEmitter::emitFloatCompare(CompareNode* n) {
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
		if(isX87Ty(n->getLHS()->getType())) {
			if(fc.swap) {
				fldX87(n->getLHS()); // -> st(1)
				fldX87(n->getRHS()); // -> st(0) (first)
			} else {
				fldX87(n->getRHS()); // -> st(1)
				fldX87(n->getLHS()); // -> st(0) (first)
			}
			a.fucomip();	// compare st(0):st(1), set EFLAGS, pop st(0)
			a.fstpReg0(); // discard the remaining operand
			storeBool(n, fc.cc);
			return;
		}
		U32 w = opWidth(n->getLHS()->getType());
		loadFloat(n->getLHS(), 0);
		loadFloat(n->getRHS(), 1);
		if(fc.swap)
			a.ucomis(w, 1, 0);
		else
			a.ucomis(w, 0, 1);
		storeBool(n, fc.cc);
	}

	void X86EmitterPass::FunctionEmitter::emitConvertX87(ConvertNode* n, Node* src, Opcode op) {
		switch(op) {
		case Opcode::FPExt: {
			if(isX87Ty(src->getType())) {
				fldX87(src);
				fstpX87Slot(n);
				return;
			}
			U32 sw = opWidth(src->getType());
			loadFloat(src, 0);
			a.storeXmm(0, RBP, ldScratch, sw);
			if(sw == 4)
				a.fldD(RBP, ldScratch);
			else
				a.fldL(RBP, ldScratch);
			fstpX87Slot(n);
			return;
		}
		case Opcode::FPTrunc: {
			fldX87(src);
			U32 dw = opWidth(n->getType());
			if(dw == 4)
				a.fstpD(RBP, ldScratch);
			else
				a.fstpL(RBP, ldScratch);
			a.loadXmm(0, RBP, ldScratch, dw);
			storeFloat(n, 0);
			return;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP: {
			loadInt(src, RAX);
			a.storeMem(RBP, ldScratch, RAX, 8);
			a.fildQ(RBP, ldScratch);
			fstpX87Slot(n);
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			fldX87(src);
			a.fnstcw(RBP, ldScratch + 8);
			a.loadExt(RAX, RBP, ldScratch + 8, 2, false);
			a.movRegImm64(RCX, 0x0c00);
			a.orRR(RAX, RCX);
			a.storeMem(RBP, ldScratch + 10, RAX, 2);
			a.fldcw(RBP, ldScratch + 10);
			a.fistpQ(RBP, ldScratch);
			a.fldcw(RBP, ldScratch + 8);
			a.load64(RAX, RBP, ldScratch);
			storeInt(n, RAX);
			return;
		}
		default:
			return;
		}
	}

	void X86EmitterPass::FunctionEmitter::emitConvert(ConvertNode* n) {
		Node* src = n->getOperand();
		Opcode op = n->getOpcode();

		if(isX87Ty(n->getType()) || isX87Ty(src->getType())) {
			emitConvertX87(n, src, op);
			return;
		}

		switch(op) {
		case Opcode::Trunc: {
			loadInt(src, RAX);
			maskToBits(intBits(n->getType()), RAX);
			storeInt(n, RAX);
			return;
		}
		case Opcode::SExt: {
			loadInt(src, RAX);
			storeInt(n, RAX);
			return;
		}
		case Opcode::ZExt: {
			U32 sw = opWidth(src->getType());
			if(ConstantNode* c = dyn_cast<ConstantNode>(src))
				a.movRegImm64(RAX, (U64)c->getValue());
			else
				a.loadExt(RAX, RBP, slotOf(src), sw, false);
			maskToBits(intBits(src->getType()), RAX);
			storeInt(n, RAX);
			return;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP: {
			U32 w = opWidth(n->getType());
			loadInt(src, RAX);
			a.cvtRR(Asm::ssePrefixByte(w), 0x2a, true, 0, RAX);
			storeFloat(n, 0);
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			U32 w = opWidth(src->getType());
			loadFloat(src, 0);
			a.cvtRR(Asm::ssePrefixByte(w), 0x2c, true, RAX, 0);
			storeInt(n, RAX);
			return;
		}
		case Opcode::FPExt: {
			loadFloat(src, 0);
			a.cvtRR(0xf3, 0x5a, false, 0, 0);
			storeFloat(n, 0);
			return;
		}
		case Opcode::FPTrunc: {
			loadFloat(src, 0);
			a.cvtRR(0xf2, 0x5a, false, 0, 0);
			storeFloat(n, 0);
			return;
		}
		default:
			return;
		}
	}

	void X86EmitterPass::FunctionEmitter::emitVaStart(CallNode* c) {
		loadInt(c->getArg(0), R10);
		a.movRegImm64(RAX, namedGp * 8);
		a.storeMem(R10, 0, RAX, 4);
		a.movRegImm64(RAX, kGpSaveBytes + namedFp * kXmmSlotBytes);
		a.storeMem(R10, 4, RAX, 4);
		a.leaMem(RAX, RBP, overflowOff);
		a.storeMem(R10, 8, RAX, 8);
		a.leaMem(RAX, RBP, saveArea);
		a.storeMem(R10, 16, RAX, 8);
	}

	void X86EmitterPass::FunctionEmitter::vaOverflow(I32 step) {
		a.load64(RCX, R10, 8);
		a.movRR(RAX, RCX);
		a.addRegImm32(RAX, step);
		a.storeMem(R10, 8, RAX, 8);
	}

	void X86EmitterPass::FunctionEmitter::vaRegOrStack(I32 offDisp, U32 limit, I32 regStep) {
		a.loadExt(RAX, R10, offDisp, 4, false);
		a.movRegImm64(RCX, limit);
		a.cmpRR(RAX, RCX);
		U32 toStack = a.jccRel32(CC_AE);
		a.load64(RCX, R10, 16);
		a.addRR(RCX, RAX);
		a.addRegImm32(RAX, regStep);
		a.storeMem(R10, offDisp, RAX, 4);
		U32 done = a.jmpRel32();
		a.patchRel32(toStack, a.here());
		vaOverflow(8);
		a.patchRel32(done, a.here());
	}

	void X86EmitterPass::FunctionEmitter::emitVaArg(CallNode* c) {
		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt = vp ? vp->getType() : nullptr;
		B32 fl = rt && rt->isFloat();
		loadInt(c->getArg(0), R10);

		if(isX87Ty(rt)) {
			vaOverflow(16);
			if(vp && slot.count(vp)) {
				a.fldT(RCX, 0);
				fstpX87Slot(vp);
			}
			return;
		}

		if(fl) {
			vaRegOrStack(4, kRegSaveBytes, (I32)kXmmSlotBytes);
			a.loadXmm(0, RCX, 0, opWidth(rt));
			if(vp && slot.count(vp))
				storeFloat(vp, 0);
			return;
		}

		vaRegOrStack(0, kGpSaveBytes, 8);
		a.loadExt(RAX, RCX, 0, opWidth(rt), rt && rt->isInt());
		if(vp && slot.count(vp))
			storeInt(vp, RAX);
	}

	void X86EmitterPass::FunctionEmitter::emitCall(CallNode* c) {
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
			if(used < max) {
				args.push_back({arg, cls, (I32)used++});
			} else {
				args.push_back({arg, cls, -1});
				stackBytes += 8;
			}
		};
		for(U32 i = 0; i < nargs; ++i) {
			Node* arg = c->getArg(i);
			if(isX87Ty(arg->getType())) {
				args.push_back({arg, X87, -1});
				stackBytes += 16;
			} else if(isFloatTy(arg->getType())) {
				classify(arg, Sse, xmmUsed, kMaxXmmArgs);
			} else {
				classify(arg, Int, intUsed, kMaxIntArgs);
			}
		}

		B32 pad = (stackBytes & 15) != 0;
		if(pad)
			a.subRegImm32(RSP, 8);
		for(U32 i = args.size(); i-- > 0;) {
			const ArgLoc& al = args[i];
			if(al.reg >= 0)
				continue;
			if(al.cls == X87) {
				a.subRegImm32(RSP, 16);
				fldX87(al.node);
				a.fstpT(RSP, 0);
			} else if(al.cls == Sse) {
				loadFloat(al.node, 0);
				a.subRegImm32(RSP, 8);
				a.storeXmm(0, RSP, 0, opWidth(al.node->getType()));
			} else {
				loadInt(al.node, RAX);
				a.push(RAX);
			}
		}

		for(const ArgLoc& al : args) {
			if(al.reg < 0)
				continue;
			if(al.cls == Sse)
				loadFloat(al.node, (U32)al.reg);
			else
				loadInt(al.node, kIntArgRegs[al.reg]);
		}

		a.movRegImm64(RAX, xmmUsed);

		if(c->isIndirect()) {
			loadInt(c->getTarget(), R11);
			a.callReg(R11);
		} else {
			a.callSym(libcName(c->getCallee()));
		}

		U32 popBytes = stackBytes + (pad ? 8 : 0);
		if(popBytes)
			a.addRegImm32(RSP, (I32)popBytes);

		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt =
				c->returnsValue() ? c->getType()->getTupleElement(CallNode::valueProjIndex()) : nullptr;
		if(isX87Ty(rt)) {
			if(vp && slot.count(vp))
				fstpX87Slot(vp);
			else
				a.fstpT(RBP, ldScratch);
		} else if(vp && slot.count(vp)) {
			if(isFloatTy(vp->getType()))
				storeFloat(vp, 0); // xmm0
			else
				storeInt(vp, RAX);
		}
	}

	void X86EmitterPass::FunctionEmitter::emitPhiCopies(I32 targetBlock, I32 predIdx) {
		const Schedule::Block& tb = sched.block(targetBlock);
		if(tb.phis.empty())
			return;

		List<PhiNode*> dests;
		for(PhiNode* phi : tb.phis) {
			Node* v = phi->getValue(predIdx);
			if(v == phi)
				continue;
			if(isX87Ty(phi->getType())) {
				a.subRegImm32(RSP, 16);
				fldX87(v);
				a.fstpT(RSP, 0);
			} else if(isFloatTy(phi->getType())) {
				if(ConstantNode* c = dyn_cast<ConstantNode>(v))
					a.movRegImm64(RAX, (U64)c->getValue());
				else
					a.load64(RAX, RBP, slotOf(v));
				a.push(RAX);
			} else {
				loadInt(v, RAX);
				a.push(RAX);
			}
			dests.push_back(phi);
		}
		for(U32 i = dests.size(); i-- > 0;) {
			PhiNode* d = dests[i];
			if(isX87Ty(d->getType())) {
				a.fldT(RSP, 0);
				a.fstpT(RBP, slotOf(d));
				a.addRegImm32(RSP, 16);
			} else {
				a.pop(RAX);
				a.storeMem(RBP, slotOf(d), RAX, 8);
			}
		}
	}

	void X86EmitterPass::FunctionEmitter::recordFix(U32 dispAt, I32 targetBlock) {
		fixes.push_back({dispAt, targetBlock});
	}

	void X86EmitterPass::FunctionEmitter::jumpTo(I32 target, I32 fallthrough) {
		if(target == fallthrough)
			return;
		recordFix(a.jmpRel32(), target);
	}

	void X86EmitterPass::FunctionEmitter::emitTerminator(I32 b, I32 fallthrough) {
		const Schedule::Block& blk = sched.block(b);
		switch(blk.term) {
		case TK::Return: {
			ReturnNode* r = cast<ReturnNode>(blk.termNode);
			if(r->hasValue()) {
				Node* v = r->getValue();
				if(isX87Ty(v->getType()))
					fldX87(v);
				else if(isFloatTy(v->getType()))
					loadFloat(v, 0);
				else
					loadInt(v, RAX);
			}
			a.leave();
			a.ret();
			return;
		}
		case TK::Branch: {
			IfNode* iff = cast<IfNode>(blk.termNode);
			loadInt(iff->getPredicate(), RAX);
			a.testRR(RAX, RAX);
			recordFix(a.jccRel32(CC_NE), blk.thenB);
			jumpTo(blk.elseB, fallthrough);
			return;
		}
		case TK::Goto: {
			emitPhiCopies(blk.gotoB, blk.gotoPredIdx);
			jumpTo(blk.gotoB, fallthrough);
			return;
		}
		}
	}

	void X86EmitterPass::emitGlobal(ElfObject& elf, const Module& mod, const Global* g) {
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

	void X86EmitterPass::emitModule(const Module& mod) {
		ElfObject elf;

		for(const Global* g : mod.globals())
			emitGlobal(elf, mod, g);

		for(const Function* fn : mod) {
			FunctionEmitter em(*fn);
			em.run();
			elf.align(ElfObject::Text, 16);
			U32 off = elf.append(ElfObject::Text, em.code.data(), (U32)em.code.size());
			elf.defineSymbol(fn->getName(), ElfObject::Text, off, true, true);
			for(const AsmReloc& r : em.relocs)
				elf.addReloc(ElfObject::Text, off + r.offset, r.symbol, r.kind, r.addend);
		}

		elf.write(*os);
	}

	X86EmitterPass::X86EmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* X86EmitterPass::name() const { return "x86-emitter"; }

	B32 X86EmitterPass::run(Module& module) {
		emitModule(module);
		return false;
	}
} // namespace rat
