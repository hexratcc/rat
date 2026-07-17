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
} // namespace rat
