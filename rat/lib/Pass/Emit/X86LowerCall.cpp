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
	List<PhysReg> X86LowerPass::callerSavedClobbers() const {
		// volatile = allocatable minus callee-saved, plus the encoder scratch regs
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

		MachineInstr call;
		call.op = (MachineOpcode)X86Op::Call;
		call.isCall = true;
		call.clobbers = callerSavedClobbers();

		if(c->isIndirect()) {
			VReg t = gpValue(c->getTarget());
			copy(MachineOperand::fixed(gpReg(R11)), MachineOperand::vr(t), detail::kGp);
		}

		// classify and materialize every argument up front
		X86ArgAssigner as(*conv);
		List<ArgLoc> args;
		I32 retTemp = 0;
		if(sret) {
			// the return value travels through a hidden pointer in the first slot
			retTemp = reserve(16);
			VReg addr = fresh(detail::kGp);
			inst(X86Op::FrameAddr, detail::kGp, {MachineOperand::vr(addr)}, {}, (I64)retTemp);
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

		if(conv->alHoldsSseCount) {
			VReg al = fresh(detail::kGp);
			def1(X86Op::LoadImm, al, detail::kGp, {MachineOperand::immVal((I64)as.sseUsed)});
			copy(MachineOperand::fixed(gpReg(RAX)), MachineOperand::vr(al), detail::kGp);
		}

		// register arguments: copy into place and pin as uses
		for(const ArgLoc& al : args) {
			if(al.reg < 0)
				continue;
			if(al.cls == Kind::Sse) {
				MachineOperand dst = MachineOperand::fixed(xmmReg((U32)al.reg), al.val.width);
				copy(dst, al.val, detail::kFp);
				call.uses.push_back(dst);
			} else {
				MachineOperand dst = MachineOperand::fixed(gpReg(conv->gpArgs[al.reg]));
				copy(dst, al.val, detail::kGp);
				call.uses.push_back(dst);
			}
		}
		if(conv->alHoldsSseCount)
			call.uses.push_back(MachineOperand::fixed(gpReg(RAX)));

		if(c->isIndirect()) {
			call.uses.push_back(MachineOperand::fixed(gpReg(R11)));
			call.imm2 = 1; // indirect
		} else {
			call.uses.push_back(MachineOperand::symbol(libcName(c->getCallee())));
		}

		// stack arguments follow the target use, in declaration order
		for(const ArgLoc& al : args)
			if(al.reg < 0)
				call.uses.push_back(al.val);
		call.imm = (I64)as.stackBytes;
		emit(call);

		// return value
		if(sret) {
			if(vp)
				inst(X86Op::X87FromSse,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(vp))},
						 {MachineOperand::frameSlot(retTemp)},
						 detail::kX87MemBits);
			return;
		}
		if(rt && isX87Ty(rt)) { // st(0) return
			if(!vp) {
				inst(X86Op::X87StoreMem, detail::kX87, {}, {}, -2); // pop the unused st(0)
				return;
			}
			inst(X86Op::X87StoreMem, detail::kX87, {MachineOperand::frameSlot(x87SlotOf(vp))}, {}, -1);
			return;
		}
		if(!vp || !rt)
			return;
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

	// copy an x87 value into a fresh 16-byte temporary and return a vreg holding its address
	VReg X86LowerPass::x87ByRefArg(Node* arg) {
		I32 src = x87Value(arg);
		I32 tmp = reserve(16);
		inst(X86Op::X87FromSse,
				 detail::kX87,
				 {MachineOperand::frameSlot(tmp)},
				 {MachineOperand::frameSlot(src)},
				 detail::kX87MemBits);
		VReg addr = fresh(detail::kGp);
		inst(X86Op::FrameAddr, detail::kGp, {MachineOperand::vr(addr)}, {}, (I64)tmp);
		return addr;
	}

	void X86LowerPass::emitPrologue() {
		StartNode* st = fn->getStart();
		using Kind = X86ArgAssigner::Kind;
		X86ArgAssigner as(*conv);

		if(conv->x87ByRef && isX87Ty(fn->getReturnType())) {
			// stash the hidden sret pointer for the return sequence
			X86ArgAssigner::Loc l = as.next(Kind::Int);
			inst(X86Op::Store,
					 detail::kGp,
					 {},
					 {MachineOperand::frameSlot(fl->sretSlot),
						MachineOperand::fixed(gpReg(conv->gpArgs[l.reg]))});
		}

		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			ProjNode* p = st->projection(StartNode::paramProjIndex(i));
			Type* t = fn->getParamType(i);
			if(isX87Ty(t)) {
				if(conv->x87ByRef) {
					// the parameter is a pointer, load the value through it
					X86ArgAssigner::Loc l = as.next(Kind::Int);
					if(!p)
						continue;
					VReg addr = fresh(detail::kGp);
					if(l.reg >= 0) {
						copy(MachineOperand::vr(addr),
								 MachineOperand::fixed(gpReg(conv->gpArgs[l.reg])),
								 detail::kGp);
					} else {
						VReg home = fresh(detail::kGp);
						inst(X86Op::FrameAddr,
								 detail::kGp,
								 {MachineOperand::vr(home)},
								 {},
								 (I64)(conv->stackParamOff + (I32)l.stackOff));
						inst(X86Op::Load, detail::kGp, {MachineOperand::vr(addr)}, {MachineOperand::vr(home)});
					}
					inst(X86Op::X87LoadMem,
							 detail::kX87,
							 {MachineOperand::frameSlot(x87SlotOf(p))},
							 {MachineOperand::vr(addr)},
							 detail::kX87MemBits);
				} else {
					// by value on the stack
					X86ArgAssigner::Loc l = as.next(Kind::X87);
					if(!p)
						continue;
					VReg addr = fresh(detail::kGp);
					inst(X86Op::FrameAddr,
							 detail::kGp,
							 {MachineOperand::vr(addr)},
							 {},
							 (I64)(conv->stackParamOff + (I32)l.stackOff));
					inst(X86Op::X87LoadMem,
							 detail::kX87,
							 {MachineOperand::frameSlot(x87SlotOf(p))},
							 {MachineOperand::vr(addr)},
							 detail::kX87MemBits);
				}
			} else if(isSseTy(t)) {
				X86ArgAssigner::Loc l = as.next(Kind::Sse);
				if(l.reg >= 0) {
					if(p) {
						U32 w = opWidth(t);
						copy(MachineOperand::vr(vregFor(p), w),
								 MachineOperand::fixed(xmmReg((U32)l.reg), w),
								 detail::kFp);
					}
				} else {
					loadStackParam(p, t, conv->stackParamOff + (I32)l.stackOff);
				}
			} else {
				X86ArgAssigner::Loc l = as.next(Kind::Int);
				if(l.reg >= 0) {
					if(p)
						copy(MachineOperand::vr(vregFor(p)),
								 MachineOperand::fixed(gpReg(conv->gpArgs[l.reg])),
								 detail::kGp);
				} else {
					loadStackParam(p, t, conv->stackParamOff + (I32)l.stackOff);
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
		needScratch(); // the fetch sequences stash through the scratch slot
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

	void X86LowerPass::emitReturn(ReturnNode* r) {
		MachineInstr m;
		m.op = (MachineOpcode)X86Op::Ret;
		if(r->hasValue()) {
			Node* v = r->getValue();
			if(isX87Ty(v->getType()) && conv->x87ByRef) {
				// write the value through the hidden sret pointer and return it in rax
				I32 s = x87Value(v);
				VReg ptr = fresh(detail::kGp);
				inst(X86Op::Load,
						 detail::kGp,
						 {MachineOperand::vr(ptr)},
						 {MachineOperand::frameSlot(fl->sretSlot)});
				inst(X86Op::X87StoreMem,
						 detail::kX87,
						 {},
						 {MachineOperand::vr(ptr), MachineOperand::frameSlot(s)},
						 detail::kX87MemBits);
				copy(MachineOperand::fixed(gpReg(RAX)), MachineOperand::vr(ptr), detail::kGp);
				m.uses = {MachineOperand::fixed(gpReg(RAX))};
			} else if(isX87Ty(v->getType())) {
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
} // namespace rat
