#include "Pass/Emit/X86Emitter.h"

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
		List<PhysReg> cl;
		if(winAbi) {
			for(Reg r : win64::kArgRegs)
				cl.push_back(gpReg(r));
			cl.push_back(gpReg(RAX));
			cl.push_back(gpReg(R10));
			cl.push_back(gpReg(R11));
			for(U32 i = 0; i < 6; ++i) // xmm0-xmm5 are volatile
				cl.push_back(xmmReg(i));
			return cl;
		}
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
		if(winAbi)
			emitCallWin64(c);
		else
			emitCallSysV(c);
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

	void X86LowerPass::emitCallWin64(CallNode* c) {
		enum ArgClass { Int, Sse };
		struct ArgLoc {
			VReg v;
			ArgClass cls;
			U32 width;
			I32 slot; // register slot index, or -1 for a stack argument
		};

		Node* vp = c->projection(CallNode::valueProjIndex());
		const Type* rt =
				c->returnsValue() ? c->getType()->getTupleElement(CallNode::valueProjIndex()) : nullptr;
		B32 sret = rt && isX87Ty(rt);

		MachineInstr call;
		call.op = (MachineOpcode)X86Op::Call;
		call.isCall = true;
		call.clobbers = callerSavedClobbers();

		if(c->isIndirect()) {
			VReg t = gpValue(c->getTarget());
			copy(MachineOperand::fixed(gpReg(R11)), MachineOperand::vr(t), detail::kGp);
		}

		U32 slot = 0;
		U32 stackBytes = 0;
		List<ArgLoc> args;
		auto place = [&](VReg v, ArgClass cls, U32 w) {
			I32 reg = slot < win64::kMaxRegArgs ? (I32)slot : -1;
			if(reg < 0)
				stackBytes += 8;
			args.push_back({v, cls, w, reg});
			++slot;
		};

		I32 retTemp = 0;
		if(sret) {
			retTemp = reserve(16);
			VReg addr = fresh(detail::kGp);
			inst(X86Op::FrameAddr, detail::kGp, {MachineOperand::vr(addr)}, {}, (I64)retTemp);
			place(addr, Int, 8);
		}
		for(U32 i = 0; i < c->getArgCount(); ++i) {
			Node* arg = c->getArg(i);
			if(isX87Ty(arg->getType()))
				place(x87ByRefArg(arg), Int, 8);
			else if(isSseTy(arg->getType()))
				place(sseValue(arg), Sse, opWidth(arg->getType()));
			else
				place(gpValue(arg), Int, 8);
		}

		for(const ArgLoc& al : args) {
			if(al.slot < 0)
				continue;
			if(al.cls == Sse) {
				copy(MachineOperand::fixed(xmmReg((U32)al.slot), al.width),
						 MachineOperand::vr(al.v, al.width),
						 detail::kFp);
				call.uses.push_back(MachineOperand::fixed(xmmReg((U32)al.slot), al.width));
			} else {
				copy(MachineOperand::fixed(gpReg(win64::kArgRegs[al.slot])),
						 MachineOperand::vr(al.v),
						 detail::kGp);
				call.uses.push_back(MachineOperand::fixed(gpReg(win64::kArgRegs[al.slot])));
			}
		}

		if(c->isIndirect()) {
			call.uses.push_back(MachineOperand::fixed(gpReg(R11)));
			call.imm2 = 1; // indirect
		} else {
			call.uses.push_back(MachineOperand::symbol(libcName(c->getCallee())));
		}

		for(const ArgLoc& al : args) {
			if(al.slot >= 0)
				continue;
			if(al.cls == Sse)
				call.uses.push_back(MachineOperand::vr(al.v, al.width));
			else
				call.uses.push_back(MachineOperand::vr(al.v, 8));
		}
		call.imm = (I64)stackBytes;
		emit(call);

		if(sret) {
			if(vp)
				inst(X86Op::X87FromSse,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(vp))},
						 {MachineOperand::frameSlot(retTemp)},
						 detail::kX87MemBits);
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

	void X86LowerPass::emitCallSysV(CallNode* c) {
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
		if(winAbi)
			emitPrologueWin64();
		else
			emitPrologueSysV();
	}

	void X86LowerPass::emitPrologueWin64() {
		StartNode* st = fn->getStart();
		U32 slot = 0;
		if(isX87Ty(fn->getReturnType())) {
			// stash the sret pointer for the return sequence
			inst(X86Op::Store,
					 detail::kGp,
					 {},
					 {MachineOperand::frameSlot(fl->sretSlot), MachineOperand::fixed(gpReg(RCX))});
			++slot;
		}
		auto slotReg = [&](U32 s) { return gpReg(win64::kArgRegs[s]); };
		auto stackDisp = [&](U32 s) { return win64::kStackParamOff + 8 * ((I32)s - 4); };
		for(U32 i = 0; i < fn->getParamCount(); ++i) {
			ProjNode* p = st->projection(StartNode::paramProjIndex(i));
			Type* t = fn->getParamType(i);
			U32 s = slot++;
			if(isX87Ty(t)) {
				if(!p)
					continue;
				VReg addr = fresh(detail::kGp);
				if(s < win64::kMaxRegArgs) {
					copy(MachineOperand::vr(addr), MachineOperand::fixed(slotReg(s)), detail::kGp);
				} else {
					VReg home = fresh(detail::kGp);
					inst(X86Op::FrameAddr, detail::kGp, {MachineOperand::vr(home)}, {}, (I64)stackDisp(s));
					inst(X86Op::Load, detail::kGp, {MachineOperand::vr(addr)}, {MachineOperand::vr(home)});
				}
				inst(X86Op::X87LoadMem,
						 detail::kX87,
						 {MachineOperand::frameSlot(x87SlotOf(p))},
						 {MachineOperand::vr(addr)},
						 detail::kX87MemBits);
			} else if(isSseTy(t)) {
				if(s < win64::kMaxRegArgs) {
					if(p) {
						U32 w = opWidth(t);
						copy(MachineOperand::vr(vregFor(p), w),
								 MachineOperand::fixed(xmmReg(s), w),
								 detail::kFp);
					}
				} else {
					loadStackParam(p, t, stackDisp(s));
				}
			} else {
				if(s < win64::kMaxRegArgs) {
					if(p)
						copy(MachineOperand::vr(vregFor(p)), MachineOperand::fixed(slotReg(s)), detail::kGp);
				} else {
					loadStackParam(p, t, stackDisp(s));
				}
			}
		}
	}

	void X86LowerPass::emitPrologueSysV() {
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
			if(isX87Ty(v->getType()) && winAbi) {
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
