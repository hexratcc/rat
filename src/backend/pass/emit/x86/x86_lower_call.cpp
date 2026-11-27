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
	using detail::kFp;
	using detail::kGp;

	// allocatable and scratch regs, scratch is never callee-saved
	U64 X86LowerPass::regClobbers(B32 withCalleeSaved) const {
		U64 cl = 0;
		for(const RegClass& rc : regs->classes) {
			for(PhysReg p : rc.allocatable)
				cl |= detail::physBit(p);
			for(PhysReg p : rc.scratch)
				cl |= detail::physBit(p);
			if(!withCalleeSaved)
				for(PhysReg p : rc.calleeSaved)
					cl &= ~detail::physBit(p);
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
		if(!isSseTy(rt) || opWidth(rt) != w)
			return false;
		Node* vp = c->projection(CallNode::valueProjIndex());
		if(!vp)
			return true;
		VReg s = sseValue(arg);
		VReg d = vregFor(vp);
		if(isSqrt)
			sse(scalarEnc(0x51, w), vr(d, w), {vr(s, w)});
		else
			put(X86Op::FAbs, {vr(d, w)}, {vr(s, w)}, (I64)w);
		return true;
	}

	void X86LowerPass::emitAsm(AsmNode* a) {
		for(U32 i = 0; i < a->getOutputCount(); ++i) {
			ProjNode* out = a->projection(AsmNode::outputProjIndex(i));
			if(!out)
				continue;
			Node* src = a->getInputOperand(i);
			U32 cls = classOf(out->getType());
			VReg s = classValue(src, cls);
			moveValue(vregFor(out), s, cls, opWidth(out->getType()));
		}
	}

	B32 X86LowerPass::emitBuiltin(CallNode* c) {
		const String& callee = c->getCallee();
		Node* vp = c->projection(CallNode::valueProjIndex());
		if(callee == "__builtin_va_start") {
			VReg ptr = gpValue(c->getArg(0));
			MachineInstr& m = put(X86Op::VaStart, {}, {vr(ptr)}, (I64)fl->namedGp, (I64)fl->namedFp);
			m.clobbers = detail::scratchBits();
		} else if(callee == "__builtin_setjmp") {
			mov(ph(R11), vr(gpValue(c->getArg(0))));
			MachineInstr& m = put(X86Op::SetJmp, {ph(RAX)}, {ph(R11)});
			m.isCall = true; // nothing survives the jump in a register
			m.clobbers = regClobbers(true);
			if(vp)
				mov(vr(vregFor(vp)), ph(RAX));
		} else if(callee == "__builtin_longjmp") {
			mov(ph(R11), vr(gpValue(c->getArg(0))));
			put(X86Op::LongJmp, {}, {ph(R11)}).clobbers = detail::physBit(detail::gpPhys(R10));
		} else if(callee == "__builtin_trap") {
			put(X86Op::Ud2, {}, {});
		} else if(callee == "__builtin_prefetch") {
			// prefetchw needs 3dnowprefetch, so a write prefetch uses the read form
			constexpr U8 kHint[] = {0, 3, 2, 1};
			I64 l = 3;
			if(c->getArgCount() >= 3)
				if(ConstantNode* loc = dyn_cast<ConstantNode>(c->getArg(2)))
					l = loc->getValue();
			if(l < 0 || l > 3)
				l = 3;
			VReg p = gpValue(c->getArg(0));
			put(X86Op::Prefetch, {}, {vr(p)}, 0, (I64)kHint[l]);
		} else if(callee == "__builtin_frame_address") {
			if(vp)
				put(X86Op::FrameAddr, {vr(vregFor(vp))}, {}, 0);
		} else if(callee == "__builtin_return_address") {
			if(vp)
				put(X86Op::RetAddr, {vr(vregFor(vp))}, {});
		} else if(callee != "__builtin_va_end") {
			return emitMathIntrinsic(c);
		}
		return true;
	}

	void X86LowerPass::emitCall(CallNode* c) {
		if(!c->isIndirect() && emitBuiltin(c))
			return;
		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt = nullptr;
		if(c->returnsValue())
			rt = c->getType()->getTupleElement(CallNode::valueProjIndex());
		B32 sret = conv->x87ByRef && isX87Ty(rt);
		VReg target = c->isIndirect() ? gpValue(c->getTarget()) : kNoVReg;
		X86ArgAssigner as(*conv);
		List<CallArg> args;
		I32 retTemp = 0;
		if(sret) {
			retTemp = reserve(16);
			VReg addr = frameAddr((I64)retTemp);
			args.push_back({vr(addr), Kind::Int, as.next(Kind::Int).reg});
		}
		// a return in registers lands in the slot arg 0 points at
		const AbiRegs& retRegs = c->getAbi().ret;
		VReg retSlot = retRegs.count ? gpValue(c->getArg(0)) : kNoVReg;
		pushCallArgs(c, as, args);
		MachineInstr call;
		call.op = (MachineOpcode)X86Op::Call;
		call.isCall = true;
		call.clobbers = regClobbers(false);
		call.uses = callUses(c, args, as.sseUsed);
		call.imm = (I64)as.stackBytes;
		if(c->isIndirect()) {
			call.imm2 = 1; // indirect
			mov(ph(R11), vr(target));
		}
		mb->insts.push_back(std::move(call));
		if(!retRegs.count)
			return callResult(vp, rt, sret, retTemp);
		moveRegReturn(retSlot, retRegs, ReturnMove::IntoSlot);
		if(vp)
			mov(vr(vregFor(vp)), vr(retSlot));
	}

	void X86LowerPass::pushCallArgs(CallNode* c, X86ArgAssigner& as, List<CallArg>& args) {
		const CallAbi& abi = c->getAbi();
		for(U32 i = abi.ret.count ? 1 : 0; i < c->getArgCount(); ++i) {
			std::optional<ByvalArg> byval = abi.byvalAt(i);
			if(!byval) {
				args.push_back(callArg(c->getArg(i), as));
				continue;
			}
			U32 stackOff = as.nextMem(byval->bytes, byval->align).stackOff;
			I64 placement = (I64)stackOff << 32 | byval->bytes;
			args.push_back({vr(gpValue(c->getArg(i))), Kind::Int, -1});
			args.push_back({im(placement), Kind::Int, -1});
		}
	}

	X86LowerPass::Ops X86LowerPass::callUses(CallNode* c, const List<CallArg>& args, U32 sseUsed) {
		Ops uses;
		for(const CallArg& a : args) {
			if(a.reg < 0)
				continue;
			B32 sse = a.cls == Kind::Sse;
			MachineOperand dst = sse ? xm((U32)a.reg, a.val.width) : ph(conv->gpArgs[a.reg]);
			mov(dst, a.val);
			uses.push_back(dst);
		}
		if(conv->alHoldsSseCount && c->isVarArgs()) {
			mov(ph(RAX), vr(gpConst(sseUsed)));
			uses.push_back(ph(RAX));
		}
		if(c->isIndirect())
			uses.push_back(ph(R11));
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
			I32 src = x87Value(arg);
			I32 tmp = reserve(16);
			fldSlot(tmp, src);
			return {vr(frameAddr((I64)tmp)), Kind::Int, as.next(Kind::Int).reg};
		}
		if(isX87Ty(t))
			return {sl(x87Value(arg), 16), Kind::X87, as.next(Kind::X87).reg};
		if(isSseTy(t))
			return {vr(sseValue(arg), opWidth(t)), Kind::Sse, as.next(Kind::Sse).reg};
		// a narrow int goes widened to a dword, as clang callees read it
		return {vr(dwordValue(arg, true)), Kind::Int, as.next(Kind::Int).reg};
	}

	void X86LowerPass::callResult(Node* vp, const Type* rt, B32 sret, I32 retTemp) {
		if(sret) {
			if(vp)
				fldSlot(x87SlotOf(vp), retTemp);
			return;
		}
		if(isX87Ty(rt)) { // st(0) return
			if(vp)
				put(X86Op::X87StoreMem, {sl(x87SlotOf(vp))}, {}, -1);
			else
				put(X86Op::X87StoreMem, {}, {}, -2); // pop the unused st(0)
			return;
		}
		if(!vp || !rt)
			return;
		VReg d = vregFor(vp);
		if(isSseTy(rt)) {
			mov(vr(d, opWidth(rt)), xm(0, opWidth(rt)));
		} else {
			mov(vr(d), ph(RAX));
			if(rt->isInt(1))
				maskBits(d, 1);
		}
	}

	// the return registers into the slot after a call, or out of it before a ret
	X86LowerPass::Ops
	X86LowerPass::moveRegReturn(VReg retSlot, const AbiRegs& regs, ReturnMove move) {
		if(regs.cls[0] == AbiClass::X87) {
			moveX87Return(retSlot, regs.count, move);
			return {};
		}
		constexpr Reg kRetGp[] = {RAX, RDX};
		Ops uses;
		U32 gp = 0;
		U32 sse = 0;
		for(U32 k = 0; k < regs.count; ++k) {
			if(regs.cls[k] == AbiClass::None) // an empty aggregate
				continue;
			U32 cls = regs.cls[k] == AbiClass::Sse ? kFp : kGp;
			MachineOperand reg;
			if(cls == kFp)
				reg = xm(sse++);
			else
				reg = ph(kRetGp[gp++]);
			AddrParts at{retSlot, (I32)(8 * k)};
			VReg v = fresh(cls);
			if(move == ReturnMove::IntoSlot) {
				mov(vr(v), reg);
				store(cls == kFp ? X86Op::FStore : X86Op::Store, at, vr(v));
				continue;
			}
			load(cls == kFp ? X86Op::FLoad : X86Op::Load, v, 8, at);
			mov(reg, vr(v));
			uses.push_back(reg);
		}
		return uses;
	}

	// st0 then st1, 16 bytes per value. the loads run in reverse so the value at 0 ends in st0
	void X86LowerPass::moveX87Return(VReg retSlot, U32 count, ReturnMove move) {
		for(U32 i = 0; i < count; ++i) {
			I32 value = reserve(16, 16);
			if(move == ReturnMove::IntoSlot) {
				put(X86Op::X87StoreMem, {sl(value)}, {}, -1);
				VReg addr = x87PartAddr(retSlot, i);
				put(X86Op::X87StoreMem, {}, {vr(addr), sl(value)}, detail::kX87MemBits);
			} else {
				VReg addr = x87PartAddr(retSlot, count - 1 - i);
				put(X86Op::X87LoadMem, {sl(value)}, {vr(addr)}, detail::kX87MemBits);
				put(X86Op::X87LoadMem, {}, {sl(value)}, -1);
			}
		}
	}

	VReg X86LowerPass::x87PartAddr(VReg base, U32 index) {
		if(!index)
			return base;
		VReg addr = fresh(kGp);
		mov(vr(addr), vr(base));
		alu(X86Op::Add, addr, im(16 * (I64)index));
		return addr;
	}

	// where each ir param arrives, the abi return slot pointer arrives nowhere
	List<X86ArgAssigner::Loc> X86LowerPass::paramLocs(X86ArgAssigner& as) {
		const CallAbi& abi = fn->getAttrs().abi;
		if(conv->x87ByRef && isX87Ty(fn->getReturnType()))
			as.next(Kind::Int); // the hidden sret pointer comes first
		List<X86ArgAssigner::Loc> locs;
		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			std::optional<ByvalArg> byval = abi.byvalAt(i);
			if(i == 0 && abi.ret.count)
				locs.push_back({-1, 0});
			else if(byval)
				locs.push_back(as.nextMem(byval->bytes, byval->align));
			else
				locs.push_back(as.next(argKind(fn->getParamType(i))));
		}
		return locs;
	}

	U32 X86LowerPass::emitPrologue() {
		X86ArgAssigner as(*conv);
		List<X86ArgAssigner::Loc> locs = paramLocs(as);
		if(conv->x87ByRef && isX87Ty(fn->getReturnType())) // stash it for the return sequence
			put(X86Op::Store, {}, {sl(fl->sretSlot), ph(conv->gpArgs[0])});
		for(U32 i = 0; i < fn->getParamCount(); ++i)
			lowerParam(i, locs[i]);
		U32 paramsEnd = (U32)mb->insts.size();
		maskBoolParams(locs);
		return paramsEnd;
	}

	// a bool param arrives with only its low byte defined
	void X86LowerPass::maskBoolParams(const List<X86ArgAssigner::Loc>& locs) {
		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			ProjNode* p = fn->getStart()->projection(StartNode::paramProjIndex(i));
			if(p && locs[i].reg >= 0 && fn->getParamType(i)->isInt(1))
				maskBits(vregFor(p), 8);
		}
	}

	// a by-value aggregate is the address of its incoming copy
	void X86LowerPass::lowerParam(U32 index, X86ArgAssigner::Loc l) {
		ProjNode* p = fn->getStart()->projection(StartNode::paramProjIndex(index));
		if(!p)
			return;
		const CallAbi& abi = fn->getAttrs().abi;
		if(index == 0 && abi.ret.count) {
			I64 retBytes = abi.ret.cls[0] == AbiClass::X87 ? 32 : 16;
			put(X86Op::FrameAddr, {vr(vregFor(p))}, {}, reserve(retBytes, 16));
			return;
		}
		if(abi.byvalAt(index)) {
			VReg addr = stackParamAddr(l.stackOff);
			mov(vr(vregFor(p)), vr(addr));
			return;
		}
		Type* t = fn->getParamType(index);
		U32 w = opWidth(t);
		if(isX87Ty(t)) {
			x87Param(p, l);
		} else if(l.reg >= 0 && isSseTy(t)) {
			mov(vr(vregFor(p), w), xm((U32)l.reg, w));
		} else if(l.reg >= 0) {
			mov(vr(vregFor(p)), ph(conv->gpArgs[l.reg]));
		} else {
			VReg addr = stackParamAddr(l.stackOff);
			X86Op op = isSseTy(t) ? X86Op::FLoad : X86Op::Load;
			load(op, vregFor(p), w, AddrParts{addr}, t && t->isInt());
		}
	}

	void X86LowerPass::x87Param(ProjNode* p, X86ArgAssigner::Loc l) {
		VReg addr;
		if(!conv->x87ByRef) {
			addr = stackParamAddr(l.stackOff);
		} else {
			addr = fresh(kGp);
			if(l.reg >= 0) {
				mov(vr(addr), ph(conv->gpArgs[l.reg]));
			} else {
				VReg home = stackParamAddr(l.stackOff);
				load(X86Op::Load, addr, 8, AddrParts{home});
			}
		}
		put(X86Op::X87LoadMem, {sl(x87SlotOf(p))}, {vr(addr)}, detail::kX87MemBits);
	}

	VReg X86LowerPass::stackParamAddr(U32 off) {
		return frameAddr((I64)(conv->stackParamOff + (I32)off));
	}

	void X86LowerPass::emitReturn(ReturnNode* r) {
		MachineInstr m;
		m.op = (MachineOpcode)X86Op::Ret;
		if(r->hasValue()) {
			Node* v = r->getValue();
			const AbiRegs& retRegs = fn->getAttrs().abi.ret;
			if(retRegs.count) {
				m.uses = moveRegReturn(gpValue(v), retRegs, ReturnMove::OutOfSlot);
			} else if(isX87Ty(v->getType()) && conv->x87ByRef) {
				// the sret pointer returns in rax
				I32 s = x87Value(v);
				VReg ptr = fresh(kGp);
				put(X86Op::Load, {vr(ptr)}, {sl(fl->sretSlot)});
				put(X86Op::X87StoreMem, {}, {vr(ptr), sl(s)}, detail::kX87MemBits);
				mov(ph(RAX), vr(ptr));
				m.uses = {ph(RAX)};
			} else if(isX87Ty(v->getType())) {
				put(X86Op::X87LoadMem, {}, {sl(x87Value(v))}, -1);
			} else if(isSseTy(v->getType())) {
				U32 w = opWidth(v->getType());
				VReg s = sseValue(v);
				mov(xm(0, w), vr(s, w));
				m.uses = {xm(0, w)};
			} else {
				VReg s = dwordValue(v, true); // like an argument, a narrow int returns widened
				mov(ph(RAX), vr(s));
				m.uses = {ph(RAX)};
			}
		}
		mb->insts.push_back(std::move(m));
	}
} // namespace rat
