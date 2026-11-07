#include "pass/emit/x86/x86_lower.h"

#include "codegen/machine_function.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "target/target.h"
#include "target/x86/x86_asm.h"

namespace rat {
	U64 X86LowerPass::callerSavedClobbers() const {
		// volatile = allocatable minus callee-saved, plus the encoder scratch regs
		U64 cl = 0;
		for(const RegClass& rc : regs->classes) {
			for(PhysReg p : rc.allocatable)
				cl |= detail::physBit(p);
			for(PhysReg p : rc.calleeSaved)
				cl &= ~detail::physBit(p);
			for(PhysReg p : rc.scratch)
				cl |= detail::physBit(p);
		}
		return cl;
	}

	U64 X86LowerPass::allRegClobbers() const {
		U64 cl = 0;
		for(const RegClass& rc : regs->classes) {
			for(PhysReg p : rc.allocatable)
				cl |= detail::physBit(p);
			for(PhysReg p : rc.scratch)
				cl |= detail::physBit(p);
		}
		return cl;
	}

	B32 X86LowerPass::emitMathIntrinsic(CallNode* c) {
		const String& callee = c->getCallee();
		B32 isSqrt = callee == "sqrt" || callee == "sqrtf";
		B32 isAbs = callee == "fabs" || callee == "fabsf";
		if((!isSqrt && !isAbs) || c->getArgCount() != 1 || !c->returnsValue())
			return false;
		U32 w = (callee == "sqrtf" || callee == "fabsf") ? 4u : 8u;
		Node* arg = c->getArg(0);
		if(!isSseTy(arg->getType()) || opWidth(arg->getType()) != w)
			return false;
		const Type* rt = c->getType()->getTupleElement(CallNode::valueProjIndex());
		if(!rt || !isSseTy(rt) || opWidth(rt) != w)
			return false;
		Node* vp = c->projection(CallNode::valueProjIndex());
		if(!vp)
			return true;
		VReg s = sseValue(arg);
		VReg d = vregFor(vp);
		if(isSqrt)
			fsqrt(d, s, w);
		else
			fabs_(d, s, w);
		return true;
	}

	void X86LowerPass::emitAsm(AsmNode* a) {
		for(U32 i = 0; i < a->getOutputCount(); ++i) {
			ProjNode* out = a->projection(AsmNode::outputProjIndex(i));
			if(!out)
				continue;
			Node* src = a->getInputOperand(i);
			U32 cls = classOf(out->getType());
			VReg s = cls == detail::kFp ? sseValue(src) : gpValue(src);
			moveValue(vregFor(out), s, cls, opWidth(out->getType()));
		}
	}

	B32 X86LowerPass::emitBuiltin(CallNode* c) {
		const String& callee = c->getCallee();
		Node* vp = c->projection(CallNode::valueProjIndex());
		if(callee == "__builtin_va_start") {
			VReg ptr = gpValue(c->getArg(0));
			vaStart(ptr, fl->namedGp, fl->namedFp);
		} else if(callee == "__builtin_va_arg") {
			emitVaArg(c);
		} else if(callee == "__builtin_va_arg_aggregate") {
			vaArgAggregate(c);
		} else if(callee == "__builtin_setjmp") {
			mov(R11, gpValue(c->getArg(0)));
			setJmp();
			if(vp)
				mov(vregFor(vp), RAX);
		} else if(callee == "__builtin_longjmp") {
			mov(R11, gpValue(c->getArg(0)));
			longJmp();
		} else if(callee == "__builtin_trap") {
			ud2();
		} else if(callee == "__builtin_prefetch") {
			// prefetchw needs 3dnowprefetch, so a write prefetch uses the read form
			static const U8 kHint[] = {0, 3, 2, 1};
			I64 l = 3;
			if(c->getArgCount() >= 3)
				if(ConstantNode* loc = dyn_cast<ConstantNode>(c->getArg(2)))
					l = loc->getValue();
			VReg p = gpValue(c->getArg(0));
			prefetch(p, kHint[l < 0 || l > 3 ? 3 : l]);
		} else if(callee == "__builtin_frame_address") {
			if(vp)
				leaFrame(vregFor(vp), 0);
		} else if(callee == "__builtin_return_address") {
			if(vp)
				retAddr(vregFor(vp));
		} else if(callee != "__builtin_va_end") {
			return emitMathIntrinsic(c);
		}
		return true;
	}

	void X86LowerPass::emitCall(CallNode* c) {
		if(!c->isIndirect() && emitBuiltin(c))
			return;

		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt =
				c->returnsValue() ? c->getType()->getTupleElement(CallNode::valueProjIndex()) : nullptr;
		B32 sret = conv->x87ByRef && rt && isX87Ty(rt);

		// detached: the argument moves are emitted while its use list is built
		MachineInstr call;
		call.op = (MachineOpcode)X86Op::Call;
		call.isCall = true;
		call.clobbers = callerSavedClobbers();

		VReg target = kNoVReg;
		if(c->isIndirect())
			target = gpValue(c->getTarget());

		// classify and materialize every argument up front
		X86ArgAssigner as(*conv);
		List<CallArg> args;
		I32 retTemp = 0;
		if(sret) {
			// the return value travels through a hidden pointer in the first slot
			retTemp = reserve(16);
			VReg addr = frameAddr((I64)retTemp);
			args.push_back({MachineOperand::vr(addr), Kind::Int, as.next(Kind::Int).reg});
		}
		// a return in registers lands in the slot arg 0 points at
		const AbiRegs& retRegs = c->getAbi().ret;
		VReg retSlot = retRegs.count ? gpValue(c->getArg(0)) : kNoVReg;
		pushCallArgs(c, as, args);

		call.uses = callUses(c, args, as.sseUsed);
		call.imm = (I64)as.stackBytes;
		if(c->isIndirect()) {
			call.imm2 = 1; // indirect
			mov(R11, target);
		}
		mb->insts.push_back(std::move(call));
		if(retRegs.count)
			storeRegReturn(vp, retSlot, retRegs);
		else
			callResult(vp, rt, sret, retTemp);
	}

	void X86LowerPass::pushCallArgs(CallNode* c, X86ArgAssigner& as, List<CallArg>& args) {
		const CallAbi& abi = c->getAbi();
		for(U32 i = abi.ret.count ? 1 : 0; i < c->getArgCount(); ++i) {
			ByvalArg byval = abi.byvalAt(i);
			if(byval.bytes)
				byvalArgs(c->getArg(i), byval, as, args);
			else
				args.push_back(callArg(c->getArg(i), as));
		}
	}

	// the copy as 8-byte stack chunks, after a padding chunk when its alignment needs one
	// a large copy is its address and then its size, the call copies it as a block
	void X86LowerPass::byvalArgs(Node* arg, ByvalArg byval, X86ArgAssigner& as, List<CallArg>& args) {
		VReg base = gpValue(arg);
		U32 unaligned = as.stackBytes;
		if(as.nextMem(byval.bytes, byval.align).stackOff != unaligned)
			args.push_back({MachineOperand::vr(base), Kind::Int, -1});
		if(byval.bytes > detail::kByvalChunkBytes) {
			args.push_back({MachineOperand::vr(base), Kind::Int, -1});
			args.push_back({MachineOperand::immVal(byval.bytes), Kind::Int, -1});
			return;
		}
		for(U32 k = 0; k < byval.bytes; k += 8) {
			VReg chunk = fresh(detail::kGp);
			ld(chunk, 8, baseDisp(base, (I32)k), false);
			args.push_back({MachineOperand::vr(chunk), Kind::Int, -1});
		}
	}

	X86LowerPass::Ops X86LowerPass::callUses(CallNode* c, const List<CallArg>& args, U32 sseUsed) {
		Ops uses;
		// register arguments: copy into place and pin as uses
		for(const CallArg& a : args)
			if(a.reg >= 0)
				uses.push_back(argToReg(a));

		if(conv->alHoldsSseCount && c->isVarArgs()) {
			VReg al = fresh(detail::kGp);
			movi(al, (I64)sseUsed);
			mov(RAX, al);
			uses.push_back(MachineOperand::fixed(gpReg(RAX)));
		}

		if(c->isIndirect())
			uses.push_back(MachineOperand::fixed(gpReg(R11)));
		else
			uses.push_back(MachineOperand::symbol(c->getCallee()));

		// stack arguments follow the target use, in declaration order
		for(const CallArg& a : args)
			if(a.reg < 0)
				uses.push_back(a.val);
		return uses;
	}

	X86LowerPass::CallArg X86LowerPass::callArg(Node* arg, X86ArgAssigner& as) {
		const Type* t = arg->getType();
		if(isX87Ty(t) && conv->x87ByRef) {
			// copy an x87 value into a fresh 16-byte temporary and return a vreg holding its address
			Slot src = x87Value(arg);
			I32 tmp = reserve(16);
			fldSlot(slot(tmp), src);
			return {MachineOperand::vr(frameAddr((I64)tmp)), Kind::Int, as.next(Kind::Int).reg};
		}
		if(isX87Ty(t))
			return {MachineOperand::frameSlot(x87Value(arg).s, 16), Kind::X87, as.next(Kind::X87).reg};
		if(isSseTy(t))
			return {MachineOperand::vr(sseValue(arg), opWidth(t)), Kind::Sse, as.next(Kind::Sse).reg};
		return {MachineOperand::vr(gpValue(arg)), Kind::Int, as.next(Kind::Int).reg};
	}

	MachineOperand X86LowerPass::argToReg(const CallArg& a) {
		if(a.cls == Kind::Sse) {
			MachineOperand dst = MachineOperand::fixed(xmmReg((U32)a.reg), a.val.width);
			mov(dst, a.val, detail::kFp);
			return dst;
		}
		MachineOperand dst = MachineOperand::fixed(gpReg(conv->gpArgs[a.reg]));
		mov(dst, a.val, detail::kGp);
		return dst;
	}

	void X86LowerPass::callResult(Node* vp, const Type* rt, B32 sret, I32 retTemp) {
		if(sret) {
			if(vp)
				fldSlot(x87SlotOf(vp), slot(retTemp));
			return;
		}
		if(rt && isX87Ty(rt)) { // st(0) return
			if(!vp) {
				fstpDiscard(); // pop the unused st(0)
				return;
			}
			fstp(x87SlotOf(vp));
			return;
		}
		if(!vp || !rt)
			return;
		VReg d = vregFor(vp);
		if(isSseTy(rt)) {
			U32 w = opWidth(rt);
			movaps(d, xmm(0, w));
		} else {
			mov(d, RAX);
			if(rt->isInt())
				signExtBits(d, intBits(rt));
		}
	}

	// rax then rdx, xmm0 then xmm1
	MachineOperand X86LowerPass::returnReg(AbiClass c, U32& gp, U32& sse) {
		if(c == AbiClass::Sse)
			return MachineOperand::fixed(xmmReg(sse++), 8);
		return MachineOperand::fixed(gpReg(gp++ ? RDX : RAX));
	}

	// the eightbytes back into the result slot, which is the call's value
	void X86LowerPass::storeRegReturn(Node* vp, VReg retSlot, const AbiRegs& regs) {
		if(regs.cls[0] == AbiClass::X87)
			return storeX87Return(vp, retSlot, regs.count);
		U32 gp = 0;
		U32 sse = 0;
		for(U32 k = 0; k < regs.count; ++k) {
			if(regs.cls[k] == AbiClass::None)
				continue;
			MachineOperand reg = returnReg(regs.cls[k], gp, sse);
			U32 cls = regs.cls[k] == AbiClass::Sse ? detail::kFp : detail::kGp;
			VReg v = fresh(cls);
			mov(MachineOperand::vr(v), reg, cls);
			if(cls == detail::kFp)
				stf(baseDisp(retSlot, (I32)(8 * k)), v, 8);
			else
				st(baseDisp(retSlot, (I32)(8 * k)), MachineOperand::vr(v));
		}
		if(vp)
			mov(vregFor(vp), retSlot);
	}

	// the eightbytes of the returned slot into their registers
	X86LowerPass::Ops X86LowerPass::loadRegReturn(Node* v, const AbiRegs& regs) {
		VReg retSlot = gpValue(v);
		if(regs.cls[0] == AbiClass::X87) {
			loadX87Return(retSlot, regs.count);
			return {};
		}
		Ops uses;
		U32 gp = 0;
		U32 sse = 0;
		for(U32 k = 0; k < regs.count; ++k) {
			if(regs.cls[k] == AbiClass::None)
				continue;
			MachineOperand reg = returnReg(regs.cls[k], gp, sse);
			U32 cls = regs.cls[k] == AbiClass::Sse ? detail::kFp : detail::kGp;
			VReg t = fresh(cls);
			if(cls == detail::kFp)
				ldf(t, 8, baseDisp(retSlot, (I32)(8 * k)));
			else
				ld(t, 8, baseDisp(retSlot, (I32)(8 * k)), false);
			mov(reg, MachineOperand::vr(t), cls);
			uses.push_back(reg);
		}
		return uses;
	}

	// st0 then st1 into the slot, 16 bytes per value
	void X86LowerPass::storeX87Return(Node* vp, VReg retSlot, U32 count) {
		for(U32 k = 0; k < count; ++k) {
			Slot value = slot(reserve(16, 16));
			fstp(value);
			fstp(x87PartAddr(retSlot, k), value);
		}
		if(vp)
			mov(vregFor(vp), retSlot);
	}

	// the value at 0 ends in st0, the one at 16 in st1
	void X86LowerPass::loadX87Return(VReg retSlot, U32 count) {
		for(U32 k = count; k-- > 0;) {
			Slot value = slot(reserve(16, 16));
			fld(value, x87PartAddr(retSlot, k));
			fldPop(value);
		}
	}

	VReg X86LowerPass::x87PartAddr(VReg base, U32 index) {
		if(!index)
			return base;
		VReg addr = fresh(detail::kGp);
		mov(addr, base);
		alu(X86Op::Add, addr, addr, imm(16 * (I64)index));
		return addr;
	}

	X86LowerPass::AddrParts X86LowerPass::baseDisp(VReg base, I32 disp) {
		AddrParts a;
		a.base = base;
		a.disp = disp;
		return a;
	}

	// static frame address in a fresh gp vreg
	VReg X86LowerPass::frameAddr(I64 disp) {
		VReg addr = fresh(detail::kGp);
		leaFrame(addr, disp);
		return addr;
	}

	// where each ir param arrives, the abi return slot pointer arrives nowhere
	List<X86ArgAssigner::Loc> X86LowerPass::paramLocs(X86ArgAssigner& as) {
		const CallAbi& abi = fn->getAttrs().abi;
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			as.next(Kind::Int); // the hidden sret pointer comes first
		List<X86ArgAssigner::Loc> locs;
		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			ByvalArg byval = abi.byvalAt(i);
			if(i == 0 && abi.ret.count)
				locs.push_back({-1, 0});
			else if(byval.bytes)
				locs.push_back(as.nextMem(byval.bytes, byval.align));
			else
				locs.push_back(as.next(argKind(fn->getParamType(i))));
		}
		return locs;
	}

	void X86LowerPass::emitPrologue() {
		X86ArgAssigner as(*conv);
		List<X86ArgAssigner::Loc> locs = paramLocs(as);
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			st(slot(fl->sretSlot), conv->gpArgs[0]); // stash it for the return sequence
		for(U32 i = 0; i < fn->getParamCount(); ++i)
			lowerParam(i, locs[i]);
	}

	// a by-value aggregate is the address of its incoming copy
	void X86LowerPass::lowerParam(U32 index, X86ArgAssigner::Loc l) {
		ProjNode* p = fn->getStart()->projection(StartNode::paramProjIndex(index));
		if(!p)
			return;
		const CallAbi& abi = fn->getAttrs().abi;
		if(index == 0 && abi.ret.count) {
			leaFrame(vregFor(p), reserve(abi.ret.cls[0] == AbiClass::X87 ? 32 : 16, 16));
			return;
		}
		if(abi.byvalAt(index).bytes) {
			mov(vregFor(p), stackParamAddr(l.stackOff));
			return;
		}
		Type* t = fn->getParamType(index);
		U32 w = opWidth(t);
		if(isX87Ty(t)) {
			x87Param(p, l);
		} else if(l.reg >= 0 && isSseTy(t)) {
			movaps(vregFor(p), xmm((U32)l.reg, w));
		} else if(l.reg >= 0) {
			mov(vregFor(p), conv->gpArgs[l.reg]);
		} else {
			VReg addr = stackParamAddr(l.stackOff);
			if(isSseTy(t))
				ldf(vregFor(p), w, addr);
			else
				ld(vregFor(p), w, addr, t && t->isInt());
		}
	}

	void X86LowerPass::x87Param(ProjNode* p, X86ArgAssigner::Loc l) {
		VReg addr;
		if(!conv->x87ByRef) {
			// by value on the stack
			addr = stackParamAddr(l.stackOff);
		} else {
			// the parameter is a pointer, load the value through it
			addr = fresh(detail::kGp);
			if(l.reg >= 0) {
				mov(addr, conv->gpArgs[l.reg]);
			} else {
				VReg home = stackParamAddr(l.stackOff);
				ld(addr, home);
			}
		}
		fld(x87SlotOf(p), addr);
	}

	VReg X86LowerPass::stackParamAddr(U32 off) {
		return frameAddr((I64)(conv->stackParamOff + (I32)off));
	}

	void X86LowerPass::emitVaArg(CallNode* c) {
		needScratch(); // the fetch sequences stash through the scratch slot
		if(!c->returnsValue())
			return;
		Node* vp = c->projection(CallNode::valueProjIndex());
		Type* rt = vp ? vp->getType() : c->getType()->getTupleElement(CallNode::valueProjIndex());
		if(!rt)
			return;
		VReg ptr = gpValue(c->getArg(0));
		VaArgKind kind = isX87Ty(rt) ? VaArgKind::X87 : (isSseTy(rt) ? VaArgKind::Sse : VaArgKind::Int);
		U32 w = opWidth(rt);
		I64 desc = (I64)w;
		if(kind == VaArgKind::Int && rt->isInt())
			desc |= (I64)1 << 32; // sign-extend the fetched value
		MachineOperand def;
		if(kind == VaArgKind::X87)
			def = MachineOperand::frameSlot(vp ? x87SlotOf(vp).s : reserve(16));
		else
			def = MachineOperand::vr(vp ? vregFor(vp) : fresh(classOf(rt)), w);
		vaArg(def, ptr, kind, desc, classOf(rt));
	}

	// the register eightbytes go through a frame temp, args 1 and 2 are the size and alignment
	void X86LowerPass::vaArgAggregate(CallNode* c) {
		needScratch();
		VaAggregate agg;
		agg.bytes = (U32)cast<ConstantNode>(c->getArg(1))->getValue();
		agg.align = (U32)cast<ConstantNode>(c->getArg(2))->getValue();
		agg.regs = c->getAbi().ret;
		Node* vp = c->projection(CallNode::valueProjIndex());
		MachineOperand def = MachineOperand::vr(vp ? vregFor(vp) : fresh(detail::kGp));
		VReg ptr = gpValue(c->getArg(0));
		MachineInstr& m =
				vaArg(def, ptr, VaArgKind::Aggregate, detail::packVaAggregate(agg), detail::kGp);
		if(agg.regs.count && conv->vaList != X86VaList::CharPtr)
			m.uses.push_back(MachineOperand::frameSlot(reserve(16), 16));
	}

	void X86LowerPass::emitReturn(ReturnNode* r) {
		MachineInstr m;
		m.op = (MachineOpcode)X86Op::Ret;
		if(r->hasValue()) {
			Node* v = r->getValue();
			const AbiRegs& retRegs = fn->getAttrs().abi.ret;
			if(retRegs.count) {
				m.uses = loadRegReturn(v, retRegs);
			} else if(isX87Ty(v->getType()) && conv->x87ByRef) {
				// write the value through the hidden sret pointer and return it in rax
				Slot s = x87Value(v);
				VReg ptr = fresh(detail::kGp);
				ld(ptr, slot(fl->sretSlot));
				fstp(ptr, s);
				mov(RAX, ptr);
				m.uses = {MachineOperand::fixed(gpReg(RAX))};
			} else if(isX87Ty(v->getType())) {
				fldPop(x87Value(v));
			} else if(isSseTy(v->getType())) {
				U32 w = opWidth(v->getType());
				VReg s = sseValue(v);
				movaps(xmm(0, w), s);
				m.uses = {MachineOperand::fixed(xmmReg(0), w)};
			} else {
				VReg s = gpValue(v);
				mov(RAX, s);
				m.uses = {MachineOperand::fixed(gpReg(RAX))};
			}
		}
		mb->insts.push_back(std::move(m));
	}
} // namespace rat
