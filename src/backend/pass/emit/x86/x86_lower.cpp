#include "pass/emit/x86/x86_lower.h"

#include <cstdio>

#include "codegen/machine_function.h"
#include "codegen/schedule.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "target/target.h"
#include "target/x86/x86_asm.h"

namespace rat {
	PhysReg X86LowerPass::gpReg(Reg r) { return detail::gpPhys(r); }
	U32 X86LowerPass::opWidth(const Type* t) {
		if(!t)
			return 8;
		if(t->isVec())
			return 16;
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

	PhysReg X86LowerPass::xmmReg(U32 n) { return detail::xmmPhys(n); }
	B32 X86LowerPass::isX87Ty(const Type* t) {
		return t && t->isFloat() && t->getFloatWidth() == 128;
	}
	B32 X86LowerPass::isSseTy(const Type* t) {
		return t && (t->isVec() || (t->isFloat() && !isX87Ty(t)));
	}
	U32 X86LowerPass::intBits(const Type* t) { return t && t->isInt() ? t->getIntWidth() : 64; }

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

	B32 X86LowerPass::branchOnlyCompare(Node* n) {
		if(!fusableCompare(n))
			return false;
		for(Node* u : n->getUsers())
			if(!isa<IfNode>(u))
				return false;
		return !n->getUsers().empty();
	}

	void X86LowerPass::needScratch() {
		if(fl->ldScratch == 0)
			fl->ldScratch = reserve(16);
	}

	I32 X86LowerPass::reserve(U32 bytes, U32 align) {
		align = std::clamp(align, 8u, 16u);
		out->frameBytes += bytes;
		out->frameBytes = (out->frameBytes + align - 1u) & ~(align - 1u);
		return -(I32)out->frameBytes;
	}

	void X86LowerPass::layout() {
		for(const Node* n : *fn)
			if(const AllocNode* al = dyn_cast<AllocNode>(n)) {
				U32 sz = std::max(al->getAllocType()->byteSize(ptrBytes), 8u);
				allocOff[n->getId()] = reserve((sz + 7u) & ~7u, al->getAlign());
			}
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			fl->sretSlot = reserve(8); // stash for the hidden x87 sret pointer
		fl->variadic = fn->getAttrs().variadic;
		if(!fl->variadic)
			return;
		needScratch(); // va_arg fetch sequences stash through the scratch slot
		X86ArgAssigner as(*conv);
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			as.next(Kind::Int); // hidden sret pointer
		for(U32 i = 0; i < fn->getParamCount(); ++i)
			as.next(argKind(fn->getParamType(i)));
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
			return detail::kX87;
		if(t && (t->isFloat() || t->isVec()))
			return detail::kFp;
		return detail::kGp;
	}

	VReg X86LowerPass::fresh(U32 cls) { return out->newVReg(cls); }

	Slot X86LowerPass::x87SlotOf(const Node* n) {
		I32& s = x87Slot[n->getId()];
		if(s == kNoSlot)
			s = reserve(16);
		return slot(s);
	}

	VReg X86LowerPass::vregFor(const Node* n) {
		VReg& v = vregOf[n->getId()];
		if(v == kNoVReg)
			v = fresh(classOf(n->getType()));
		return v;
	}

	VReg X86LowerPass::gpValue(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			U64 v = (U64)c->getValue();
			if(n->getType() && n->getType()->isInt())
				v = (U64)signExtend((I64)c->getValue(), opWidth(n->getType()) * 8);
			VReg d = fresh(detail::kGp);
			movi(d, (I64)v);
			return d;
		}
		if(GlobalNode* g = dyn_cast<GlobalNode>(n)) {
			if(vregOf[n->getId()] != kNoVReg)
				return vregOf[n->getId()]; // materialized once at its scheduled block
			VReg d = fresh(detail::kGp);
			lea(d, g->getSymbol());
			return d;
		}
		if(isa<AllocNode>(n))
			return frameAddr(allocOff[n->getId()]);
		return vregFor(n);
	}

	U32 X86LowerPass::log2Scale(I64 c) {
		if(c == 2)
			return 1;
		if(c == 4)
			return 2;
		if(c == 8)
			return 3;
		return 0;
	}

	BinaryNode* X86LowerPass::asAdd(Node* n) {
		BinaryNode* b = dyn_cast<BinaryNode>(n);
		if(!b || b->getOpcode() != Opcode::Add)
			return nullptr;
		return b;
	}

	B32 X86LowerPass::scaleOf(Node* n, Node*& idx, U32& scaleLog2) {
		BinaryNode* b = dyn_cast<BinaryNode>(n);
		if(!b || opWidth(n->getType()) != 8)
			return false;
		I64 c = 0;
		Node* x = nullptr;
		U32 sc = 0;
		if(b->getOpcode() == Opcode::Shl && immOf(b->getRHS(), c) && c >= 1 && c <= 3) {
			x = b->getLHS();
			sc = (U32)c;
		} else if(b->getOpcode() == Opcode::Mul) {
			if(immOf(b->getRHS(), c))
				x = b->getLHS();
			else if(immOf(b->getLHS(), c))
				x = b->getRHS();
			sc = log2Scale(c);
		}
		if(!x || sc == 0)
			return false;
		idx = x;
		scaleLog2 = sc;
		return true;
	}

	I64 X86LowerPass::sibBits(I64 sign, const AddrParts& a) {
		return (sign & 1) | (a.hasIndex ? 2 : 0) | ((I64)(a.scaleLog2 & 3) << 2);
	}

	X86LowerPass::AddrMatch X86LowerPass::decodeAddr(Node* ptr) {
		AddrMatch m;
		Node* work = ptr;
		I64 c;
		if(BinaryNode* add = asAdd(work)) {
			if(immOf(add->getRHS(), c)) {
				m.disp = (I32)c;
				work = add->getLHS();
			} else if(immOf(add->getLHS(), c)) {
				m.disp = (I32)c;
				work = add->getRHS();
			}
		}
		m.base = work;
		BinaryNode* add = asAdd(work);
		if(!add)
			return m;
		Node* ops[2] = {add->getRHS(), add->getLHS()};
		for(U32 i = 0; i < 2; ++i) {
			if(!scaleOf(ops[i], m.index, m.scaleLog2))
				continue;
			m.base = ops[1 - i];
			m.scaleNode = ops[i];
			m.hasIndex = true;
			return m;
		}
		return m;
	}

	X86LowerPass::AddrParts X86LowerPass::matchAddr(Node* ptr) {
		AddrMatch m = decodeAddr(ptr);
		AddrParts a;
		a.disp = m.disp;
		a.scaleLog2 = m.scaleLog2;
		a.hasIndex = m.hasIndex;
		if(AllocNode* al = dyn_cast<AllocNode>(m.base)) {
			a.frameBase = true;
			a.disp += allocOff[al->getId()];
		} else {
			a.base = gpValue(m.base);
		}
		if(m.hasIndex)
			a.index = gpValue(m.index);
		return a;
	}

	MachineOperand X86LowerPass::addrBase(const AddrParts& a) {
		if(a.frameBase)
			return MachineOperand::fixed(gpReg(RBP));
		return MachineOperand::vr(a.base);
	}

	B32 X86LowerPass::addressOnlyAdd(Node* n) {
		BinaryNode* add = asAdd(n);
		if(!add || n->getUsers().empty())
			return false;
		AddrMatch m = decodeAddr(n);
		I64 c;
		if(!m.hasIndex && !immOf(add->getRHS(), c) && !immOf(add->getLHS(), c))
			return false;
		for(Node* u : n->getUsers()) {
			// x87 memory ops carry the operand width in imm and cannot fold a
			// displacement or index, so they still need the add materialized
			if(LoadNode* ld = dyn_cast<LoadNode>(u)) {
				if(ld->getPointer() != n || isX87Ty(ld->getType()))
					return false;
			} else if(StoreNode* st = dyn_cast<StoreNode>(u)) {
				if(st->getPointer() != n || st->getValue() == n || isX87Ty(st->getValue()->getType()))
					return false;
			} else if(isa<BinaryNode>(u)) {
				// ptr + const chains decompose n through the user's own addr match
				if(!addressOnlyAdd(u))
					return false;
				AddrMatch um = decodeAddr(u);
				if(um.base == n || um.index == n)
					return false;
			} else {
				return false;
			}
		}
		return true;
	}

	B32 X86LowerPass::addressOnlyScale(Node* n) {
		Node* idx = nullptr;
		U32 sc = 0;
		if(!scaleOf(n, idx, sc) || n->getUsers().empty())
			return false;
		for(Node* u : n->getUsers())
			if(!addressOnlyAdd(u) || decodeAddr(u).scaleNode != n)
				return false;
		return true;
	}

	VReg X86LowerPass::sseValue(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			if(vregOf[n->getId()] != kNoVReg)
				return vregOf[n->getId()]; // materialized once at its scheduled block
			VReg d = fresh(detail::kFp);
			fpConstLoad(c, d);
			return d;
		}
		return vregFor(n);
	}

	String X86LowerPass::fpPoolSym(U64 bits, U32 width) {
		C8 buf[40];
		std::snprintf(buf, sizeof buf, "__rat_fp%u_%016lx", width, bits);
		String name(buf);
		if(!mod->getGlobal(name)) {
			List<U8> init(width);
			for(U32 i = 0; i < width; ++i)
				init[i] = (U8)(bits >> (8 * i));
			Global* g = mod->createGlobal(name, mod->getFloat(width * 8), true, std::move(init));
			g->setLinkage(Global::Linkage::Internal);
		}
		return name;
	}

	// rip-relative load from the constant pool
	void X86LowerPass::fpConstLoad(ConstantNode* c, VReg dst) {
		U32 w = opWidth(c->getType());
		ldf(dst, w, fpPoolSym((U64)c->getValue(), w));
	}

	Slot X86LowerPass::x87Value(Node* n) {
		Slot s = x87SlotOf(n);
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			needScratch();
			fldImm(s, (U64)c->getValue());
		}
		return s;
	}

	void X86LowerPass::emitNode(Node* n) {
		Opcode op = n->getOpcode();
		switch(op) {
		case Opcode::Global:
			lea(vregFor(n), cast<GlobalNode>(n)->getSymbol());
			return;
		case Opcode::Constant:
			if(isSseTy(n->getType()))
				fpConstLoad(cast<ConstantNode>(n), vregFor(n));
			return;
		case Opcode::Store:
			emitStore(cast<StoreNode>(n));
			return;
		case Opcode::Load:
			emitLoad(cast<LoadNode>(n));
			return;
		case Opcode::Call:
			emitCall(cast<CallNode>(n));
			return;
		case Opcode::Asm:
			emitAsm(cast<AsmNode>(n));
			return;
		case Opcode::Alloc:
			return;
		case Opcode::StackAlloc: {
			VReg sz = gpValue(cast<StackAllocNode>(n)->getSize());
			stackAlloc(vregFor(n), sz);
			return;
		}
		case Opcode::StackSave:
			stackSave(vregFor(n));
			return;
		case Opcode::StackRestore:
			stackRestore(gpValue(cast<StackRestoreNode>(n)->getSaved()));
			return;
		case Opcode::Splat:
			emitSplat(cast<SplatNode>(n));
			return;
		case Opcode::Extract:
			emitExtract(cast<ExtractNode>(n));
			return;
		case Opcode::Pack:
			emitPack(cast<PackNode>(n));
			return;
		case Opcode::Shuffle: {
			VReg v = sseValue(cast<ShuffleNode>(n)->getVector());
			vshuf(vregFor(n), v, cast<ShuffleNode>(n)->getSelector());
			return;
		}
		case Opcode::Select:
			emitSelect(cast<SelectNode>(n));
			return;
		default:
			break;
		}
		if(isCompareOpcode(op)) {
			if(branchOnlyCompare(n))
				return; // no value users; each If re-emits the compare fused with its jcc
			if(selectOnlyCompare(n))
				return; // each select re-emits it fused with its cmov
			emitCompare(cast<CompareNode>(n));
		} else if(isConvertOpcode(op)) {
			emitConvert(cast<ConvertNode>(n));
		} else if(isUnaryOpcode(op)) {
			emitUnary(cast<UnaryNode>(n));
		} else if(isBinaryOpcode(op)) {
			if(addressOnlyAdd(n))
				return; // folded into base+index*scale+disp of every using load/store
			if(addressOnlyScale(n))
				return; // folded into the SIB scale of every using address
			emitBinary(cast<BinaryNode>(n));
		}
	}

	void X86LowerPass::moveValue(VReg dst, VReg src, U32 cls, U32 w) {
		if(cls == detail::kFp)
			movaps(dst, src, w);
		else
			mov(dst, src);
	}

	// parallel-move semantics
	void X86LowerPass::emitPhiCopies(I32 targetBlock, I32 predIdx) {
		const Schedule::Block& tb = sched->block(targetBlock);
		List<Pair<PhiNode*, VReg>> moves;
		List<Pair<PhiNode*, Slot>> x87Moves;
		for(PhiNode* phi : tb.phis) {
			Node* v = phi->getValue(predIdx);
			if(v == phi)
				continue;
			U32 cls = classOf(phi->getType());
			if(cls == detail::kX87) {
				Slot s = x87Value(v);
				needScratch();
				Slot t = slot(reserve(16));
				fldSlot(t, s);
				x87Moves.push_back({phi, t});
				continue;
			}
			VReg t = fresh(cls);
			moveValue(t, cls == detail::kFp ? sseValue(v) : gpValue(v), cls, opWidth(phi->getType()));
			moves.push_back({phi, t});
		}
		for(const auto& [phi, t] : moves)
			moveValue(vregFor(phi), t, classOf(phi->getType()), opWidth(phi->getType()));
		for(const auto& [phi, t] : x87Moves)
			fldSlot(x87SlotOf(phi), t);
	}

	void X86LowerPass::emitTerminator(I32 b) {
		const Schedule::Block& blk = sched->block(b);
		switch(blk.term) {
		case Schedule::TermKind::Return:
			emitReturn(cast<ReturnNode>(blk.termNode));
			return;
		case Schedule::TermKind::Branch: {
			Node* pred = cast<IfNode>(blk.termNode)->getPredicate();
			if(fusableCompare(pred)) {
				// fuse the compare into the branch: cmp lhs, rhs; jcc
				U8 cc = emitCmp(cast<CompareNode>(pred));
				jcc(cc, blk.thenB, blk.elseB);
				return;
			}
			VReg p = gpValue(pred);
			br(p, blk.thenB, blk.elseB);
			return;
		}
		case Schedule::TermKind::Switch: {
			// per-edge trampoline blocks carry phi copies, so the table jump is direct
			VReg sel = gpValue(cast<SwitchNode>(blk.termNode)->getSelector());
			switchJump(sel, blk.caseB);
			return;
		}
		case Schedule::TermKind::Goto:
			emitPhiCopies(blk.gotoB, blk.gotoPredIdx);
			jmp(blk.gotoB);
			return;
		}
	}

	void X86LowerPass::lowerBlocks() {
		layout();
		const List<I32>& order = sched->rpo();
		out->blocks.assign(sched->numBlocks(), {});
		for(U32 i = 0; i < order.size(); ++i) {
			I32 b = order[i];
			MachineBlock& block = out->blocks[b];
			block.id = b;
			block.loopDepth = sched->block(b).loopDepth;
			block.insts.reserve(sched->block(b).nodes.size() * 2 + 4);
			mb = &block;
			if(i == 0)
				emitPrologue();
			for(Node* n : sched->block(b).nodes)
				emitNode(n);
			emitTerminator(b);
		}
		for(I32 b : order)
			for(I32 s : sched->successors(b)) {
				out->blocks[b].succs.push_back(s);
				out->blocks[s].preds.push_back(b);
			}

		// x87 sequences stage through r10/r11 in the encoder; declare so the
		// allocator can use them elsewhere
		for(MachineBlock& blk : out->blocks)
			for(MachineInstr& in : blk.insts)
				if((X86Op)in.op >= X86Op::X87LoadMem && (X86Op)in.op <= X86Op::X87Cmp) {
					in.clobbers.push_back(gpReg(R10));
					in.clobbers.push_back(gpReg(R11));
				}
	}

	B32 X86LowerPass::run(Module& module,
												const Function& f,
												MachineFunc& mf,
												const TargetInfo& target) {
		mod = &module;
		lowerFn(f, mf, target);
		return true;
	}

	void X86LowerPass::lowerFn(const Function& f, MachineFunc& mf, const TargetInfo& target) {
		conv = &x86CallConv(target.getTriple().os);
		regs = target.registers();
		ptrBytes = target.getPointerSizeInBytes();
		sse41 = target.hasSse41();
		Schedule s(f);
		X86FrameLayout frame;
		fn = &f;
		sched = &s;
		out = &mf;
		fl = &frame;
		mb = nullptr;
		vregOf.assign(f.idBound(), kNoVReg);
		x87Slot.assign(f.idBound(), kNoSlot);
		allocOff.assign(f.idBound(), kNoSlot);
		lowerBlocks();
		mf.aux = std::make_unique<X86FrameLayout>(frame); // the layout rides along on mf.aux
	}

	RegAllocHooks X86Target::regAllocHooks() const {
		RegAllocHooks hooks;
		hooks.makeReload = [](PhysReg dst, I32 slot, U32 cls, U32 width) {
			MachineInstr m;
			m.op = (MachineOpcode)(cls == detail::kFp ? X86Op::FLoad : X86Op::Load);
			m.regClass = cls;
			m.defs = {MachineOperand::fixed(dst, width)};
			m.uses = {MachineOperand::frameSlot(slot, width)};
			return m;
		};
		hooks.makeSpill = [](I32 slot, PhysReg src, U32 cls, U32 width) {
			MachineInstr m;
			m.op = (MachineOpcode)(cls == detail::kFp ? X86Op::FStore : X86Op::Store);
			m.regClass = cls;
			m.uses = {MachineOperand::frameSlot(slot, width), MachineOperand::fixed(src, width)};
			return m;
		};
		hooks.allocSlot = [](MachineFunc& fn, U32 /*cls*/, U32 width) {
			fn.frameBytes += width < 8 ? 8 : width;
			fn.frameBytes = (fn.frameBytes + 7u) & ~7u;
			return -(I32)fn.frameBytes;
		};
		hooks.isCopy = [](const MachineInstr& in) {
			return (x86OpInfo((X86Op)in.op).flags & kOpCopy) != 0;
		};
		hooks.isRemat = [](const MachineInstr& in) {
			if(x86OpInfo((X86Op)in.op).flags & kOpRemat)
				return true;
			// constant-pool load
			return in.op == (MachineOpcode)X86Op::FLoad && in.uses.size() == 1 &&
						 in.uses[0].kind == MachineOperand::Kind::Sym;
		};
		return hooks;
	}

} // namespace rat
