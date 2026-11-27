#include "pass/emit/x86/x86_lower.h"

#include <cstdio>

#include "codegen/machine_function.h"
#include "codegen/schedule.h"
#include "hash.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "target/target.h"
#include "target/x86/x86_asm.h"

namespace rat {
	using detail::kFp;
	using detail::kGp;
	using detail::kX87;

	U32 X86LowerPass::opWidth(const Type* t) {
		if(!t)
			return 8;
		if(t->isVec()) // 16, or 8 or 4 bytes for a vector in the low lanes
			return t->getVecLanes() * opWidth(t->getVecElement());
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

	B32 X86LowerPass::repeatsLanes(const Type* t) {
		return t && t->isVec() && t->getVecLanes() == 2 && t->getVecElement()->isFloat() &&
					 t->getVecElement()->getFloatWidth() == 32;
	}

	B32 X86LowerPass::readsLowLanes(const Node* n) {
		for(Node* u : n->getUsers()) {
			B32 stored = isa<StoreNode>(u) && cast<StoreNode>(u)->getValue() == n;
			B32 lowOnly = isa<ExtractNode>(u) || isa<ShuffleNode>(u) || u->getOpcode() == Opcode::FPExt;
			if(!stored && !lowOnly)
				return false;
		}
		return true;
	}

	B32 X86LowerPass::gpSignOp(const Node* n) {
		Opcode op = n->getOpcode();
		const Type* t = n->getType();
		if((op != Opcode::FAbs && op != Opcode::FNeg) || !t->isFloat() || isX87Ty(t))
			return false;
		const Node* operand = cast<UnaryNode>(n)->getOperand();
		if(!isa<LoadNode>(operand) || operand->getUsers().size() != 1)
			return false;
		for(Node* u : n->getUsers()) {
			const StoreNode* s = dyn_cast<StoreNode>(u);
			if(!s || s->getValue() != n || s->getPointer() == n)
				return false;
		}
		return n->hasUsers();
	}

	B32 X86LowerPass::gpSignLoad(const LoadNode* l) {
		return l->getUsers().size() == 1 && gpSignOp(l->getUsers()[0]);
	}

	// a vector moves and spills as the whole xmm, so the repeated lanes of a <2 x f32> survive a reload
	U32 X86LowerPass::regWidth(const Type* t) { return t && t->isVec() ? 16 : opWidth(t); }

	B32 X86LowerPass::isX87Ty(const Type* t) {
		return t && t->isFloat() && t->getFloatWidth() == 128;
	}
	B32 X86LowerPass::isSseTy(const Type* t) {
		return t && (t->isVec() || (t->isFloat() && !isX87Ty(t)));
	}

	U32 X86LowerPass::intBits(const Type* t) { return t && t->isInt() ? t->getIntWidth() : 64; }

	// operand width of shifts, neg and narrow compares, the bits above an int's width are undefined
	U32 X86LowerPass::gpWidth(const Type* t) { return intBits(t) > 32 ? 8 : 4; }

	B32 X86LowerPass::immOf(Node* n, I64& out) {
		ConstantNode* c = dyn_cast<ConstantNode>(n);
		if(!c || !n->getType()->isInt())
			return false;
		I64 v = c->getValue();
		if(v != (I64)(I32)v)
			return false;
		out = v;
		return true;
	}

	B32 X86LowerPass::fusableCompare(Node* n) {
		Opcode op = n->getOpcode();
		if(!isCompareOpcode(op))
			return false;
		if(op < Opcode::FEq)
			return true;
		return op >= Opcode::FLt && !isX87Ty(cast<CompareNode>(n)->getLHS()->getType());
	}

	// every user is a select reading n as its condition only
	B32 X86LowerPass::selectOnlyCompare(Node* n) {
		if(!fusableCompare(n) || n->getUsers().empty())
			return false;
		for(Node* u : n->getUsers()) {
			SelectNode* s = dyn_cast<SelectNode>(u);
			// an arm reading the compare as a value still needs it materialized
			if(!s || s->getCondition() != n || s->getTrue() == n || s->getFalse() == n)
				return false;
		}
		return true;
	}

	void X86LowerPass::needScratch() {
		if(fl->ldScratch == 0)
			fl->ldScratch = reserve(16);
	}

	I32 X86LowerPass::reserve(U32 bytes, U32 align) {
		align = std::clamp(align, 8u, 16u);
		out->frameBytes += bytes;
		out->frameBytes = (U32)alignUp(out->frameBytes, align);
		return -(I32)out->frameBytes;
	}

	void X86LowerPass::layout() {
		for(const Node* n : sched->allocNodes()) {
			const AllocNode* al = cast<AllocNode>(n);
			U32 sz = std::max(al->getAllocType()->byteSize(ptrBytes), 8u);
			fl->allocOff[n->getId()] = reserve((U32)alignUp(sz, 8), al->getAlign());
		}
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			fl->sretSlot = reserve(8); // stash for the hidden x87 sret pointer
		fl->variadic = fn->getAttrs().variadic;
		if(!fl->variadic)
			return;
		X86ArgAssigner as(*conv);
		paramLocs(as);
		if(conv->vaList == X86VaList::CharPtr) {
			fl->namedGp = as.slot;
			fl->overflowOff = conv->homeOff + 8 * (I32)as.slot;
			return;
		}
		fl->namedGp = as.gpUsed;
		fl->namedFp = as.sseUsed;
		fl->overflowOff = conv->stackParamOff + (I32)as.stackBytes;
		fl->saveArea = reserve(conv->regSaveBytes);
	}

	X86LowerPass::Kind X86LowerPass::argKind(const Type* t) const {
		if(isX87Ty(t))
			return conv->x87ByRef ? Kind::Int : Kind::X87;
		return isSseTy(t) ? Kind::Sse : Kind::Int;
	}

	U32 X86LowerPass::classOf(const Type* t) const {
		if(isX87Ty(t))
			return kX87;
		return isSseTy(t) ? kFp : kGp;
	}

	VReg X86LowerPass::fresh(U32 cls) { return out->newVReg(cls); }

	I32 X86LowerPass::x87SlotOf(const Node* n) {
		I32& s = x87Slot[n->getId()];
		if(s == detail::kNoSlot)
			s = reserve(16);
		return s;
	}

	VReg X86LowerPass::vregFor(const Node* n) {
		VReg& v = vregOf[n->getId()];
		if(v == kNoVReg)
			v = fresh(classOf(n->getType()));
		return v;
	}

	VReg X86LowerPass::gpValue(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			I64 v = c->getValue();
			if(n->getType() && n->getType()->isInt())
				v = signExtend(v, opWidth(n->getType()) * 8);
			return gpConst(v);
		}
		if(GlobalNode* g = dyn_cast<GlobalNode>(n)) {
			if(vregOf[n->getId()] != kNoVReg)
				return vregOf[n->getId()]; // materialized once at its scheduled block
			VReg d = fresh(kGp);
			put(X86Op::LoadSym, {vr(d)}, {MachineOperand::symbol(g->getSymbol())});
			return d;
		}
		if(isa<AllocNode>(n))
			return frameAddr(fl->allocOff[n->getId()]);
		return vregFor(n);
	}

	VReg X86LowerPass::classValue(Node* n, U32 cls) { return cls == kFp ? sseValue(n) : gpValue(n); }

	VReg X86LowerPass::sseValue(Node* n) {
		ConstantNode* c = dyn_cast<ConstantNode>(n);
		if(!c)
			return vregFor(n);
		if(vregOf[n->getId()] != kNoVReg)
			return vregOf[n->getId()]; // materialized once at its scheduled block
		VReg d = fresh(kFp);
		loadFp(d, (U64)c->getValue(), opWidth(c->getType()));
		return d;
	}

	I32 X86LowerPass::x87Value(Node* n) {
		I32 s = x87SlotOf(n);
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			needScratch();
			put(X86Op::X87LoadImmD, {sl(s)}, {im(c->getValue())});
		}
		return s;
	}

	VReg X86LowerPass::gpConst(I64 v) {
		VReg t = fresh(kGp);
		put(X86Op::LoadImm, {vr(t)}, {im(v)});
		return t;
	}

	VReg X86LowerPass::frameAddr(I64 disp) {
		VReg addr = fresh(kGp);
		put(X86Op::FrameAddr, {vr(addr)}, {}, disp);
		return addr;
	}

	// rip-relative load of a constant pool global, created once per module
	void X86LowerPass::loadPool(VReg d, U32 w, const String& name, const List<U8>& bytes, B32 isVec) {
		Global* g = mod->getGlobal(name);
		if(!g) {
			Type* t = isVec ? mod->getArray(mod->getInt(8), 16) : mod->getFloat(w * 8);
			g = mod->createGlobal(name, t, true, bytes);
			g->setLinkage(Global::Linkage::Internal);
		}
		// the global name outlives the machine code that names it
		put(X86Op::FLoad, {vr(d, w)}, {MachineOperand::symbol(g->getName())});
	}

	void X86LowerPass::loadFp(VReg d, U64 bits, U32 w) {
		C8 name[40];
		std::snprintf(name, sizeof name, "__rat_fp%u_%016lx", w, bits);
		List<U8> bytes(w);
		for(U32 i = 0; i < w; ++i)
			bytes[i] = (U8)(bits >> (8 * i));
		loadPool(d, w, name, bytes, false);
	}

	void X86LowerPass::loadVec(VReg d, const List<U8>& bytes) {
		U64 h = kFnvBasis;
		for(U8 x : bytes)
			hashMix(h, x);
		C8 name[48];
		std::snprintf(name, sizeof name, "__rat_vec_%016lx", (U64)h);
		loadPool(d, 16, name, bytes, true);
	}

	void X86LowerPass::emitNode(Node* n) {
		Opcode op = n->getOpcode();
		switch(op) {
		case Opcode::Global: {
			VReg d = vregFor(n);
			put(X86Op::LoadSym, {vr(d)}, {MachineOperand::symbol(cast<GlobalNode>(n)->getSymbol())});
			return;
		}
		case Opcode::Constant:
			if(isSseTy(n->getType()) && !onlyUsedBy(n, Opcode::Pack))
				loadFp(vregFor(n), (U64)cast<ConstantNode>(n)->getValue(), opWidth(n->getType()));
			return;
		case Opcode::Store:
			return emitStore(cast<StoreNode>(n));
		case Opcode::Load:
			return emitLoad(cast<LoadNode>(n));
		case Opcode::Call:
			return emitCall(cast<CallNode>(n));
		case Opcode::Asm:
			return emitAsm(cast<AsmNode>(n));
		case Opcode::StackAlloc: {
			VReg sz = gpValue(cast<StackAllocNode>(n)->getSize());
			MachineInstr& m = put(X86Op::StackAlloc, {vr(vregFor(n))}, {vr(sz)});
			m.clobbers = detail::scratchBits(); // rounds through the scratch regs
			return;
		}
		case Opcode::StackSave:
			put(X86Op::StackSave, {vr(vregFor(n))}, {});
			return;
		case Opcode::StackRestore: {
			VReg saved = gpValue(cast<StackRestoreNode>(n)->getSaved());
			put(X86Op::StackRestore, {}, {vr(saved)});
			return;
		}
		case Opcode::Splat:
			return emitSplat(cast<SplatNode>(n));
		case Opcode::Extract:
			return emitExtract(cast<ExtractNode>(n));
		case Opcode::Pack:
			return emitPack(cast<PackNode>(n));
		case Opcode::Shuffle:
			return emitShuffle(cast<ShuffleNode>(n));
		case Opcode::Select:
			return emitSelect(cast<SelectNode>(n));
		default:
			break;
		}
		if(isCompareOpcode(op)) {
			if(n->getType()->isVec())
				return; // a lane mask, each vector select re-emits it
			// each If or select re-emits it fused with its jcc or cmov
			if((fusableCompare(n) && onlyUsedBy(n, Opcode::If)) || selectOnlyCompare(n))
				return;
			emitCompare(cast<CompareNode>(n));
		} else if(isConvertOpcode(op)) {
			emitConvert(cast<ConvertNode>(n));
		} else if(isUnaryOpcode(op)) {
			emitUnary(cast<UnaryNode>(n));
		} else if(isBinaryOpcode(op)) {
			// folded into each using address or addsub shuffle
			if(addressOnlyAdd(n) || addressOnlyScale(n) || addSubOnly(n))
				return;
			emitBinary(cast<BinaryNode>(n));
		}
	}

	void X86LowerPass::moveValue(VReg dst, VReg src, U32 cls, U32 w) {
		U32 width = cls == kFp ? w : 8;
		mov(vr(dst, width), vr(src, width));
	}

	void X86LowerPass::extendAtDef(const Node* n) {
		VReg v = vregOf[n->getId()];
		if(v == kNoVReg || !sextAtDef(n))
			return;
		extendBits(v, intBits(n->getType()), true);
		extOf[n->getId()] = Ext::Sign;
	}

	// phis copy in parallel, through temps
	void X86LowerPass::emitPhiCopies(I32 targetBlock, I32 predIdx) {
		List<Pair<PhiNode*, VReg>> phiMoves;
		List<Pair<PhiNode*, I32>> x87Moves;
		for(Node* n : sched->phis(targetBlock)) {
			PhiNode* phi = cast<PhiNode>(n);
			Node* v = phi->getValue(predIdx);
			if(v == phi)
				continue;
			U32 cls = classOf(phi->getType());
			if(cls == kX87) {
				I32 s = x87Value(v);
				needScratch();
				I32 t = reserve(16);
				fldSlot(t, s);
				x87Moves.push_back({phi, t});
				continue;
			}
			VReg t = fresh(cls);
			moveValue(t, classValue(v, cls), cls, regWidth(phi->getType()));
			if(sextAtDef(phi)) {
				if(!holdsExt(v, Ext::Sign))
					extendBits(t, intBits(phi->getType()), true);
				extOf[phi->getId()] = Ext::Sign;
			}
			phiMoves.push_back({phi, t});
		}
		for(const auto& [phi, t] : phiMoves)
			moveValue(vregFor(phi), t, classOf(phi->getType()), regWidth(phi->getType()));
		for(const auto& [phi, t] : x87Moves)
			fldSlot(x87SlotOf(phi), t);
	}

	void X86LowerPass::emitTerminator(I32 b) {
		const Schedule::Block& blk = sched->block(b);
		switch(blk.term) {
		case Schedule::TermKind::Return:
			return emitReturn(cast<ReturnNode>(blk.termNode));
		case Schedule::TermKind::Branch: {
			Node* pred = cast<IfNode>(blk.termNode)->getPredicate();
			MachineOperand thenBlock = MachineOperand::blockRef(blk.succs[0]);
			MachineOperand elseBlock = MachineOperand::blockRef(blk.succs[1]);
			if(fusableCompare(pred)) {
				U8 cc = emitCmp(cast<CompareNode>(pred));
				put(X86Op::Br, {}, {thenBlock, elseBlock}, (I64)cc, 1); // cc in imm, no predicate
				return;
			}
			VReg p = gpValue(pred);
			put(X86Op::Br, {}, {vr(p), thenBlock, elseBlock});
			return;
		}
		case Schedule::TermKind::Switch: {
			// per-edge trampoline blocks carry phi copies, so the table jump is direct
			VReg sel = gpValue(cast<SwitchNode>(blk.termNode)->getSelector());
			Ops uses = {vr(sel)};
			for(I32 t : blk.succs)
				uses.push_back(MachineOperand::blockRef(t));
			put(X86Op::SwitchJump, {}, std::move(uses)).clobbers = detail::scratchBits();
			return;
		}
		case Schedule::TermKind::Goto:
			emitPhiCopies(blk.succs[0], blk.gotoPredIdx);
			put(X86Op::Jmp, {}, {MachineOperand::blockRef(blk.succs[0])});
			return;
		}
	}

	void X86LowerPass::lowerBlocks() {
		layout();
		const List<I32>& order = sched->rpo();
		out->blocks.resize(sched->numBlocks());
		BindingPlan plan;
		B32 bind = fn->getDebug() && !fn->getDebug()->bindings.empty();
		if(bind)
			planBindings(plan);
		for(U32 i = 0; i < order.size(); ++i) {
			I32 b = order[i];
			MachineBlock& block = out->blocks[b];
			block.id = b;
			block.loopDepth = sched->block(b).loopDepth;
			block.insts.reserve(sched->nodes(b).size() * 2 + 4);
			block.succs = sched->block(b).succs;
			block.preds.reserve(sched->predCount(b));
			mb = &block;
			if(i == 0) {
				plan.paramsEnd = emitPrologue();
				plan.prologueEnd = (U32)block.insts.size();
			}
			for(Node* n : sched->nodes(b)) {
				curLoc = n->getLoc();
				emitNode(n);
				extendAtDef(n);
				if(bind)
					plan.nodeEnd[n->getId()] = (U32)block.insts.size();
			}
			Node* term = sched->block(b).termNode;
			curLoc = term ? term->getLoc() : 0;
			emitTerminator(b);
			if(bind)
				placeBindings(b, plan);
		}
		for(I32 b : order)
			for(I32 s : sched->block(b).succs)
				out->blocks[s].preds.push_back(b);
	}

	B32 X86LowerPass::run(Module& module,
												const Function& f,
												MachineFunc& mf,
												const TargetInfo& target) {
		mod = &module;
		conv = &x86CallConv(target.getTriple().os);
		regs = target.registers();
		ptrBytes = target.getPointerSizeInBytes();
		Schedule s(f);
		X86FrameLayout frame;
		fn = &f;
		sched = &s;
		out = &mf;
		fl = &frame;
		vregOf.assign(f.idBound(), kNoVReg);
		extOf.assign(f.idBound(), Ext::None);
		x87Slot.assign(f.idBound(), detail::kNoSlot);
		curLoc = 0;
		frame.allocOff.assign(f.idBound(), detail::kNoSlot);
		lowerBlocks();
		mf.aux = std::make_unique<X86FrameLayout>(std::move(frame));
		return true;
	}

	namespace detail {
		// a gp slot always moves all 8 bytes
		void x86Reload(MachineInstr& m, PhysReg dst, I32 slot, U32 cls, U32 width) {
			m.op = (MachineOpcode)(cls == kFp ? X86Op::FLoad : X86Op::Load);
			U32 moveWidth = cls == kFp ? width : 8;
			m.defs = {MachineOperand::fixed(dst, moveWidth)};
			m.uses = {MachineOperand::frameSlot(slot, moveWidth)};
		}

		void x86Spill(MachineInstr& m, I32 slot, PhysReg src, U32 cls, U32 width) {
			m.op = (MachineOpcode)(cls == kFp ? X86Op::FStore : X86Op::Store);
			U32 moveWidth = cls == kFp ? width : 8;
			m.uses = {MachineOperand::frameSlot(slot, moveWidth), MachineOperand::fixed(src, moveWidth)};
		}

		I32 x86AllocSlot(MachineFunc& fn, U32, U32 width) {
			fn.frameBytes += std::max(width, 8u);
			fn.frameBytes = (U32)alignUp(fn.frameBytes, 8);
			return -(I32)fn.frameBytes;
		}

		B32 x86IsCopy(const MachineInstr& in) { return in.op == (MachineOpcode)X86Op::Copy; }
	} // namespace detail

	RegAllocHooks X86Target::regAllocHooks() const {
		return {detail::x86Reload, detail::x86Spill, detail::x86AllocSlot, detail::x86IsCopy};
	}
} // namespace rat
