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

	B32 X86LowerPass::isIntCompare(Node* n) {
		Opcode op = n->getOpcode();
		return isCompareOpcode(op) && op < Opcode::FEq;
	}

	B32 X86LowerPass::branchOnlyCompare(Node* n) {
		if(!isIntCompare(n))
			return false;
		for(Node* u : n->getUsers())
			if(!isa<IfNode>(u))
				return false;
		return !n->getUsers().empty();
	}

	String X86LowerPass::libcName(const String& callee) {
		if(callee.rfind("__builtin_", 0) == 0)
			return callee.substr(10);
		return callee;
	}

	void X86LowerPass::reset(const Function& f, Schedule& s, MachineFunc& o, X86FrameLayout& layout) {
		fn = &f;
		sched = &s;
		out = &o;
		fl = &layout;
		vregOf.clear();
		x87Slot.clear();
		allocOff.clear();
		mb = nullptr;
	}

	void X86LowerPass::needScratch() {
		if(fl->ldScratch == 0)
			fl->ldScratch = reserve(16);
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
					U32 sz = al->getAllocType()->byteSize(ptrBytes);
					if(sz == 0)
						sz = 8;
					sz = (sz + 7u) & ~7u;
					allocOff[n] = reserve(sz);
				}
			}
		}
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			fl->sretSlot = reserve(8); // stash for the hidden x87 sret pointer
		fl->variadic = fn->isVariadic();
		if(fl->variadic) {
			needScratch(); // va_arg fetch sequences stash through the scratch slot
			layoutVariadic();
		}
	}

	void X86LowerPass::layoutVariadic() {
		using Kind = X86ArgAssigner::Kind;
		X86ArgAssigner as(*conv);
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			as.next(Kind::Int); // hidden sret pointer
		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			Type* t = fn->getParamType(i);
			if(isX87Ty(t))
				as.next(conv->x87ByRef ? Kind::Int : Kind::X87);
			else
				as.next(isSseTy(t) ? Kind::Sse : Kind::Int);
		}
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
			if(auto it = vregOf.find(n); it != vregOf.end())
				return it->second; // materialized once at its scheduled block
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

	B32 X86LowerPass::scaleOf(Node* n, Node*& idx, U32& scaleLog2) {
		BinaryNode* b = dyn_cast<BinaryNode>(n);
		if(!b)
			return false;
		if(opWidth(n->getType()) != 8)
			return false;
		I64 c;
		if(b->getOpcode() == Opcode::Shl) {
			if(immOf(b->getRHS(), c) && c >= 1 && c <= 3) {
				idx = b->getLHS();
				scaleLog2 = (U32)c;
				return true;
			}
			return false;
		}
		if(b->getOpcode() == Opcode::Mul) {
			Node* other = nullptr;
			if(immOf(b->getRHS(), c))
				other = b->getLHS();
			else if(immOf(b->getLHS(), c))
				other = b->getRHS();
			if(!other)
				return false;
			if(c == 2)
				scaleLog2 = 1;
			else if(c == 4)
				scaleLog2 = 2;
			else if(c == 8)
				scaleLog2 = 3;
			else
				return false;
			idx = other;
			return true;
		}
		return false;
	}

	I64 X86LowerPass::sibBits(I64 sign, const AddrParts& a) {
		return (sign & 1) | (a.hasIndex ? 2 : 0) | ((I64)(a.scaleLog2 & 3) << 2);
	}

	X86LowerPass::AddrMatch X86LowerPass::decodeAddr(Node* ptr) {
		AddrMatch m;
		m.base = ptr;
		Node* work = ptr;
		if(BinaryNode* add = dyn_cast<BinaryNode>(work)) {
			if(add->getOpcode() == Opcode::Add) {
				I64 c;
				if(immOf(add->getRHS(), c)) {
					m.disp = (I32)c;
					work = add->getLHS();
				} else if(immOf(add->getLHS(), c)) {
					m.disp = (I32)c;
					work = add->getRHS();
				}
			}
		}
		if(BinaryNode* add = dyn_cast<BinaryNode>(work)) {
			if(add->getOpcode() == Opcode::Add) {
				Node* idx = nullptr;
				U32 sc = 0;
				if(scaleOf(add->getRHS(), idx, sc)) {
					m.base = add->getLHS();
					m.index = idx;
					m.scaleNode = add->getRHS();
					m.scaleLog2 = sc;
					m.hasIndex = true;
					return m;
				}
				if(scaleOf(add->getLHS(), idx, sc)) {
					m.base = add->getRHS();
					m.index = idx;
					m.scaleNode = add->getLHS();
					m.scaleLog2 = sc;
					m.hasIndex = true;
					return m;
				}
			}
		}
		m.base = work;
		return m;
	}

	X86LowerPass::AddrParts X86LowerPass::matchAddr(Node* ptr) {
		AddrMatch m = decodeAddr(ptr);
		AddrParts a;
		a.disp = m.disp;
		a.scaleLog2 = m.scaleLog2;
		a.hasIndex = m.hasIndex;
		a.base = gpValue(m.base);
		if(m.hasIndex)
			a.index = gpValue(m.index);
		return a;
	}

	B32 X86LowerPass::addressOnlyAdd(Node* n) {
		BinaryNode* add = dyn_cast<BinaryNode>(n);
		if(!add || add->getOpcode() != Opcode::Add)
			return false;
		AddrMatch m = decodeAddr(n);
		I64 c;
		if(!m.hasIndex && !immOf(add->getRHS(), c) && !immOf(add->getLHS(), c))
			return false;
		if(n->getUsers().empty())
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
			} else {
				return false;
			}
		}
		return true;
	}

	B32 X86LowerPass::addressOnlyScale(Node* n) {
		Node* idx = nullptr;
		U32 sc = 0;
		if(!scaleOf(n, idx, sc))
			return false;
		if(n->getUsers().empty())
			return false;
		for(Node* u : n->getUsers()) {
			BinaryNode* add = dyn_cast<BinaryNode>(u);
			if(!add || add->getOpcode() != Opcode::Add)
				return false;
			if(!addressOnlyAdd(add))
				return false;
			if(decodeAddr(add).scaleNode != n)
				return false;
		}
		return true;
	}

	VReg X86LowerPass::sseValue(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n)) {
			if(auto it = vregOf.find(n); it != vregOf.end())
				return it->second; // materialized once at its scheduled block
			U32 w = opWidth(n->getType());
			VReg d = fresh(detail::kFp);
			needScratch(); // FLoad-of-immediate materializes through the scratch slot
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
			needScratch();
			inst(X86Op::X87LoadImmD,
					 detail::kX87,
					 {MachineOperand::frameSlot(s)},
					 {MachineOperand::immVal((I64)(U64)c->getValue())});
			return s;
		}
		return x87SlotOf(n);
	}

	void X86LowerPass::emitNode(Node* n) {
		switch(n->getOpcode()) {
		case Opcode::Global: {
			GlobalNode* g = cast<GlobalNode>(n);
			def1(X86Op::LoadSym, vregFor(n), detail::kGp, {MachineOperand::symbol(g->getSymbol())});
			return;
		}
		case Opcode::Constant: {
			ConstantNode* c = cast<ConstantNode>(n);
			if(isSseTy(n->getType())) {
				U32 w = opWidth(n->getType());
				needScratch();
				inst(X86Op::FLoad,
						 detail::kFp,
						 {MachineOperand::vr(vregFor(n), w)},
						 {MachineOperand::immVal((I64)(U64)c->getValue(), w)});
			}
			return;
		}
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
			if(branchOnlyCompare(n))
				return; // no value users; each If re-emits the compare fused with its jcc
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
			if(addressOnlyAdd(n))
				return; // folded into base+index*scale+disp of every using load/store
			if(addressOnlyScale(n))
				return; // folded into the SIB scale of every using address
			emitBinary(cast<BinaryNode>(n));
			return;
		}
	}

	void X86LowerPass::emitPhiCopies(I32 targetBlock, I32 predIdx) {
		// parallel-move semantics
		auto mov = [&](VReg dst, VReg src, U32 cls, U32 w) {
			if(cls == detail::kFp)
				copy(MachineOperand::vr(dst, w), MachineOperand::vr(src, w), detail::kFp);
			else
				copy(MachineOperand::vr(dst), MachineOperand::vr(src), detail::kGp);
		};

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
				needScratch();
				inst(X86Op::X87FromSse,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(phi))},
						 {MachineOperand::frameSlot(s)},
						 detail::kX87MemBits);
				continue;
			}
			VReg t = fresh(cls);
			mov(t, cls == detail::kFp ? sseValue(v) : gpValue(v), cls, opWidth(phi->getType()));
			live.push_back(phi);
			tmp.push_back(t);
		}
		for(U32 i = 0; i < (U32)live.size(); ++i) {
			PhiNode* phi = live[i];
			U32 cls = classOf(phi->getType());
			mov(vregFor(phi), tmp[i], cls, opWidth(phi->getType()));
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
			Node* pred = iff->getPredicate();
			if(isIntCompare(pred)) {
				// fuse the compare into the branch: cmp lhs, rhs; jcc
				CompareNode* c = cast<CompareNode>(pred);
				VReg lhs = gpValue(c->getLHS());
				I64 iv;
				if(immOf(c->getRHS(), iv)) {
					inst(X86Op::Cmp, detail::kGp, {}, {MachineOperand::vr(lhs), MachineOperand::immVal(iv)});
				} else {
					VReg rhs = gpValue(c->getRHS());
					inst(X86Op::Cmp, detail::kGp, {}, {MachineOperand::vr(lhs), MachineOperand::vr(rhs)});
				}
				inst(X86Op::Br,
						 detail::kGp,
						 {},
						 {MachineOperand::blockRef(blk.thenB), MachineOperand::blockRef(blk.elseB)},
						 (I64)detail::kIntCc[(U32)c->getOpcode() - (U32)Opcode::Eq],
						 1); // imm2 = 1: condition code in imm, no predicate register
				return;
			}
			VReg p = gpValue(pred);
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

	U32 X86LowerPass::runOnMachineFunction(const Function& fn,
																				 MachineFunc& mf,
																				 const TargetInfo& target) {
		conv = &x86CallConv(target.getTriple().os);
		regs = target.registers();
		ptrBytes = target.getPointerSizeInBytes();
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
		hooks.isCopy = [](const MachineInstr& in) { return in.op == (MachineOpcode)X86Op::Copy; };
		hooks.isRemat = [](const MachineInstr& in) {
			return in.op == (MachineOpcode)X86Op::LoadImm || in.op == (MachineOpcode)X86Op::LoadSym;
		};
		return hooks;
	}

} // namespace rat
