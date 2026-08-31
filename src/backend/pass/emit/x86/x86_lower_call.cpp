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
	List<PhysReg> X86LowerPass::callerSavedClobbers() const {
		// volatile = allocatable minus callee-saved, plus the encoder scratch regs
		static thread_local const RegisterInfo* cachedRegs = nullptr;
		static thread_local List<PhysReg> cached;
		if(cachedRegs == regs)
			return cached;

		List<PhysReg> cl;
		for(const RegClass& rc : regs->classes) {
			for(PhysReg p : rc.allocatable) {
				B32 saved = false;
				for(PhysReg s : rc.calleeSaved)
					saved |= s == p;
				if(!saved)
					cl.push_back(p);
			}
			for(PhysReg p : rc.scratch)
				cl.push_back(p);
		}
		cached = std::move(cl);
		cachedRegs = regs;
		return cached;
	}

	List<PhysReg> X86LowerPass::allRegClobbers() const {
		List<PhysReg> cl;
		for(const RegClass& rc : regs->classes) {
			for(PhysReg p : rc.allocatable)
				cl.push_back(p);
			for(PhysReg p : rc.scratch)
				cl.push_back(p);
		}
		return cl;
	}

	B32 X86LowerPass::emitMathIntrinsic(CallNode* c) {
		const String& callee = c->getCallee();
		B32 isSqrt = callee == "sqrt" || callee == "sqrtf";
		B32 isAbs = callee == "fabs" || callee == "fabsf";
		if(!isSqrt && !isAbs)
			return false;
		U32 w = (callee == "sqrtf" || callee == "fabsf") ? 4u : 8u;
		if(c->getArgCount() != 1)
			return false;
		Node* arg = c->getArg(0);
		if(!arg->getType() || !isSseTy(arg->getType()) || opWidth(arg->getType()) != w)
			return false;
		if(!c->returnsValue())
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

	B32 X86LowerPass::emitInlineIntrinsic(CallNode* c) {
		const String& callee = c->getCallee();
		if(callee == "__builtin_trap") {
			ud2();
			return true;
		}
		if(callee == "__builtin_prefetch") {
			// prefetchw needs 3dnowprefetch, so a write prefetch uses the read form
			static const U8 kHint[] = {0, 3, 2, 1};
			I64 l = 3;
			if(c->getArgCount() >= 3)
				if(ConstantNode* loc = dyn_cast<ConstantNode>(c->getArg(2)))
					l = loc->getValue();
			VReg p = gpValue(c->getArg(0));
			prefetch(p, kHint[l < 0 || l > 3 ? 3 : l]);
			return true;
		}
		B32 isFrame = callee == "__builtin_frame_address";
		if(!isFrame && callee != "__builtin_return_address")
			return false;
		Node* vp = c->projection(CallNode::valueProjIndex());
		if(!vp)
			return true;
		VReg d = vregFor(vp);
		if(isFrame)
			leaFrame(d, 0);
		else
			retAddr(d);
		return true;
	}

	void X86LowerPass::emitAsm(AsmNode* a) {
		for(U32 i = 0; i < a->getOutputCount(); ++i) {
			ProjNode* out = a->projection(AsmNode::outputProjIndex(i));
			if(!out)
				continue;
			Node* src = a->getInputOperand(i);
			if(isSseTy(out->getType())) {
				U32 w = opWidth(out->getType());
				VReg s = sseValue(src);
				movaps(vregFor(out), s, w);
			} else {
				VReg s = gpValue(src);
				mov(vregFor(out), s);
			}
		}
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
			if(callee == "__builtin_setjmp") {
				emitSetJmp(c);
				return;
			}
			if(callee == "__builtin_longjmp") {
				emitLongJmp(c);
				return;
			}
			if(emitMathIntrinsic(c))
				return;
			if(emitInlineIntrinsic(c))
				return;
		}

		using Kind = X86ArgAssigner::Kind;
		struct ArgLoc {
			MachineOperand val; // vreg, or a 16-byte frame slot for by-value x87
			Kind cls;
			I32 reg;
		};

		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt =
				c->returnsValue() ? c->getType()->getTupleElement(CallNode::valueProjIndex()) : nullptr;
		B32 sret = conv->x87ByRef && rt && isX87Ty(rt);

		// detached: the argument moves are emitted while its use list is built
		MachineInstr call;
		call.op = (MachineOpcode)X86Op::Call;
		call.isCall = true;
		call.clobbers = callerSavedClobbers();

		if(c->isIndirect()) {
			VReg t = gpValue(c->getTarget());
			mov(R11, t);
		}

		// classify and materialize every argument up front
		X86ArgAssigner as(*conv);
		List<ArgLoc> args;
		I32 retTemp = 0;
		if(sret) {
			// the return value travels through a hidden pointer in the first slot
			retTemp = reserve(16);
			VReg addr = frameAddr((I64)retTemp);
			args.push_back({MachineOperand::vr(addr), Kind::Int, as.next(Kind::Int).reg});
		}
		for(U32 i = 0; i < c->getArgCount(); ++i) {
			Node* arg = c->getArg(i);
			const Type* t = arg->getType();
			if(isX87Ty(t)) {
				if(conv->x87ByRef)
					args.push_back({MachineOperand::vr(x87ByRefArg(arg)), Kind::Int, as.next(Kind::Int).reg});
				else
					args.push_back(
							{MachineOperand::frameSlot(x87Value(arg), 16), Kind::X87, as.next(Kind::X87).reg});
			} else if(isSseTy(t)) {
				args.push_back(
						{MachineOperand::vr(sseValue(arg), opWidth(t)), Kind::Sse, as.next(Kind::Sse).reg});
			} else {
				args.push_back({MachineOperand::vr(gpValue(arg)), Kind::Int, as.next(Kind::Int).reg});
			}
		}

		// register arguments: copy into place and pin as uses
		for(const ArgLoc& a : args) {
			if(a.reg < 0)
				continue;
			if(a.cls == Kind::Sse) {
				MachineOperand dst = MachineOperand::fixed(xmmReg((U32)a.reg), a.val.width);
				mov(dst, a.val, detail::kFp);
				call.uses.push_back(dst);
			} else {
				MachineOperand dst = MachineOperand::fixed(gpReg(conv->gpArgs[a.reg]));
				mov(dst, a.val, detail::kGp);
				call.uses.push_back(dst);
			}
		}

		if(conv->alHoldsSseCount && c->isVarArgs()) {
			VReg al = fresh(detail::kGp);
			movi(al, (I64)as.sseUsed);
			mov(RAX, al);
			call.uses.push_back(MachineOperand::fixed(gpReg(RAX)));
		}

		if(c->isIndirect()) {
			call.uses.push_back(MachineOperand::fixed(gpReg(R11)));
			call.imm2 = 1; // indirect
		} else {
			call.uses.push_back(MachineOperand::symbol(c->getCallee()));
		}

		// stack arguments follow the target use, in declaration order
		for(const ArgLoc& al : args)
			if(al.reg < 0)
				call.uses.push_back(al.val);
		call.imm = (I64)as.stackBytes;
		emit(std::move(call));

		// return value
		if(sret) {
			if(vp)
				x87Move(x87SlotOf(vp), retTemp);
			return;
		}
		if(rt && isX87Ty(rt)) { // st(0) return
			if(!vp) {
				fstpDiscard(); // pop the unused st(0)
				return;
			}
			fstp(slot(x87SlotOf(vp)));
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

	// static frame address in a fresh gp vreg
	VReg X86LowerPass::frameAddr(I64 disp) {
		VReg addr = fresh(detail::kGp);
		leaFrame(addr, disp);
		return addr;
	}

	// copy an x87 value into a fresh 16-byte temporary and return a vreg holding its address
	VReg X86LowerPass::x87ByRefArg(Node* arg) {
		I32 src = x87Value(arg);
		I32 tmp = reserve(16);
		x87Move(tmp, src);
		return frameAddr((I64)tmp);
	}

	void X86LowerPass::emitPrologue() {
		StartNode* start = fn->getStart();
		using Kind = X86ArgAssigner::Kind;
		X86ArgAssigner as(*conv);

		if(conv->x87ByRef && isX87Ty(fn->getReturnType())) {
			// stash the hidden sret pointer for the return sequence
			X86ArgAssigner::Loc l = as.next(Kind::Int);
			st(slot(fl->sretSlot), conv->gpArgs[l.reg]);
		}

		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			ProjNode* p = start->projection(StartNode::paramProjIndex(i));
			Type* t = fn->getParamType(i);
			if(isX87Ty(t)) {
				// addr holds the long-double source address; load its value onto the x87 stack
				VReg addr;
				if(conv->x87ByRef) {
					// the parameter is a pointer, load the value through it
					X86ArgAssigner::Loc l = as.next(Kind::Int);
					if(!p)
						continue;
					addr = fresh(detail::kGp);
					if(l.reg >= 0) {
						mov(addr, conv->gpArgs[l.reg]);
					} else {
						VReg home = frameAddr((I64)(conv->stackParamOff + (I32)l.stackOff));
						ld(addr, home);
					}
				} else {
					// by value on the stack
					X86ArgAssigner::Loc l = as.next(Kind::X87);
					if(!p)
						continue;
					addr = frameAddr((I64)(conv->stackParamOff + (I32)l.stackOff));
				}
				fld(slot(x87SlotOf(p)), addr);
			} else if(isSseTy(t)) {
				X86ArgAssigner::Loc l = as.next(Kind::Sse);
				if(l.reg >= 0) {
					if(p) {
						U32 w = opWidth(t);
						movaps(vregFor(p), xmm((U32)l.reg, w));
					}
				} else {
					loadStackParam(p, t, conv->stackParamOff + (I32)l.stackOff);
				}
			} else {
				X86ArgAssigner::Loc l = as.next(Kind::Int);
				if(l.reg >= 0) {
					if(p)
						mov(vregFor(p), conv->gpArgs[l.reg]);
				} else {
					loadStackParam(p, t, conv->stackParamOff + (I32)l.stackOff);
				}
			}
		}
	}

	void X86LowerPass::loadStackParam(ProjNode* p, Type* t, I32 disp) {
		if(!p)
			return;
		VReg addr = frameAddr((I64)disp);
		U32 w = opWidth(t);
		if(isSseTy(t))
			ldf(vregFor(p), w, addr);
		else
			ld(vregFor(p), w, addr, t && t->isInt());
	}

	void X86LowerPass::emitVaStart(CallNode* c) {
		VReg ptr = gpValue(c->getArg(0));
		vaStart(ptr, fl->namedGp, fl->namedFp);
	}

	void X86LowerPass::emitSetJmp(CallNode* c) {
		VReg buf = gpValue(c->getArg(0));
		mov(R11, buf);
		setJmp();
		if(Node* vp = c->projection(CallNode::valueProjIndex()))
			mov(vregFor(vp), RAX);
	}

	void X86LowerPass::emitLongJmp(CallNode* c) {
		VReg buf = gpValue(c->getArg(0));
		mov(R11, buf);
		longJmp();
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
			def = MachineOperand::frameSlot(vp ? x87SlotOf(vp) : reserve(16));
		else
			def = MachineOperand::vr(vp ? vregFor(vp) : fresh(classOf(rt)), w);
		U32 cls = kind == VaArgKind::X87 ? detail::kX87 : classOf(rt);
		vaArg(def, ptr, kind, desc, cls);
	}

	void X86LowerPass::emitReturn(ReturnNode* r) {
		MachineInstr m;
		m.op = (MachineOpcode)X86Op::Ret;
		if(r->hasValue()) {
			Node* v = r->getValue();
			if(isX87Ty(v->getType()) && conv->x87ByRef) {
				// write the value through the hidden sret pointer and return it in rax
				I32 s = x87Value(v);
				VReg ptr = fresh(detail::kGp);
				ld(ptr, slot(fl->sretSlot));
				fstp(ptr, slot(s));
				mov(RAX, ptr);
				m.uses = {MachineOperand::fixed(gpReg(RAX))};
			} else if(isX87Ty(v->getType())) {
				I32 s = x87Value(v);
				fldPop(slot(s));
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
		emit(std::move(m));
	}
} // namespace rat
