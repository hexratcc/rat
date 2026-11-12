#include "pass/emit/x86/x86_lower.h"

#include "codegen/reg_alloc.h"
#include "ir/function.h"
#include "ir/node.h"

namespace rat {
	void X86LowerPass::planBindings(BindingPlan& plan) const {
		const DebugBody& body = *fn->getDebug();
		List<Pair<U32, U32>> byBlock; // (anchor block, binding)
		for(U32 i = 0; i < body.bindings.size(); ++i) {
			const Node* anchor = body.bindings[i].anchor;
			I32 b = anchor ? sched->anchorBlock(anchor) : -1;
			if(b >= 0)
				byBlock.push_back({(U32)b, i});
		}
		detail::groupBy(byBlock, (U32)sched->numBlocks(), plan.first, plan.order);
		plan.nodeEnd.assign(fn->idBound(), 0);
		plan.cursor.assign(body.vars.size(), 0);
	}

	// past the code of n when this block has it
	U32 X86LowerPass::endIn(const Node* n, I32 b, const BindingPlan& plan) const {
		return n && sched->blockOf(n) == b ? plan.nodeEnd[n->getId()] : 0;
	}

	// past a call anchor and the effect before it in the source, after the var's last record
	U32 X86LowerPass::sourcePos(const DebugBinding& bd, I32 b, const BindingPlan& plan) const {
		U32 pos = std::max(plan.cursor[bd.inst], endIn(bd.after, b, plan));
		if(const ProjNode* p = dyn_cast<ProjNode>(bd.anchor))
			pos = std::max(pos, endIn(p->getProducer(), b, plan));
		return pos;
	}

	// past the code of the value when this block has it
	U32 X86LowerPass::valuePos(const DebugBinding& bd, I32 b, const BindingPlan& plan) const {
		const Node* v = bd.value;
		if(!v || isa<PhiNode>(v) || isa<ConstantNode>(v)) // constants may sit in another function
			return 0;
		const ProjNode* vp = dyn_cast<ProjNode>(v);
		if(vp && isa<StartNode>(vp->getProducer())) // params are copied in by the prologue
			return b == sched->rpo()[0] ? plan.prologueEnd : 0;
		return std::max(endIn(v, b, plan), vp ? endIn(vp->getProducer(), b, plan) : 0);
	}

	// a vreg, an integer constant or nothing
	MachineOperand X86LowerPass::bindingLoc(const Node* value) const {
		MachineOperand none = MachineOperand::none();
		if(!value)
			return none;
		const Type* t = value->getType();
		if(const ConstantNode* c = dyn_cast<ConstantNode>(value))
			return t->isInt() || t->isPtr() ? im(imm(c->getValue(), opWidth(t))) : none;
		VReg v = vregOf[value->getId()];
		return v == kNoVReg ? none : vr(v, opWidth(t));
	}

	// records hang off the instr they precede, outside the instr lists
	void X86LowerPass::placeBindings(I32 b, BindingPlan& plan) {
		const DebugBody& body = *fn->getDebug();
		for(U32 k = plan.first[b]; k < plan.first[b + 1]; ++k) {
			const DebugBinding& bd = body.bindings[plan.order[k]];
			U32 src = sourcePos(bd, b, plan);
			U32 pos = std::max(src, valuePos(bd, b, plan));
			plan.cursor[bd.inst] = pos;
			if(pos > src) // the old value is gone, the new one not there yet
				out->addDbg(mb->dbgBefore(src), bd.inst, bindingLoc(nullptr));
			out->addDbg(mb->dbgBefore(pos), bd.inst, bindingLoc(bd.value));
		}
		for(U32 k = plan.first[b]; k < plan.first[b + 1]; ++k)
			plan.cursor[body.bindings[plan.order[k]].inst] = 0;
	}
} // namespace rat
