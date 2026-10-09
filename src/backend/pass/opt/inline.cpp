#include "pass/opt/inline.h"

#include "analysis/call_graph.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	B32 InlinePass::isStartProj(const Function& callee, Node* n) {
		ProjNode* p = dyn_cast<ProjNode>(n);
		return p && p->getProducer() == callee.getStart();
	}

	Node* InlinePass::incomingForStartProj(CallNode* call, U32 startProjIdx) {
		if(startProjIdx == StartNode::controlProjIndex())
			return call->getControl();
		if(startProjIdx == StartNode::memoryProjIndex())
			return call->getMemory();
		U32 a = startProjIdx - StartNode::paramProjIndex(0);
		return a < call->getArgCount() ? call->getArg(a) : nullptr;
	}

	B32 InlinePass::isBodyNode(const Function& callee, Node* n) {
		return n != callee.getStart() && n != callee.getStop() && n->getOpcode() != Opcode::Return;
	}

	void InlinePass::mapNode(Node* key, Node* val) {
		U32 id = key->getId();
		if(id >= cloneMap.size())
			cloneMap.resize(id + 1, nullptr);
		cloneMap[id] = val;
	}

	Node* InlinePass::mapped(Node* key) const {
		U32 id = key->getId();
		return id < cloneMap.size() ? cloneMap[id] : nullptr;
	}

	Node* InlinePass::resolve(Node* n) const {
		if(!n)
			return n;
		Node* m = mapped(n);
		return m ? m : n;
	}

	B32 InlinePass::cloneBody(Function& caller, CallNode* call, Function& callee) {
		cloneMap.clear();
		for(ProjNode* p : usersOfType<ProjNode>(callee.getStart())) {
			Node* incoming = incomingForStartProj(call, p->getIndex());
			if(!incoming)
				return false;
			mapNode(p, incoming);
		}

		// shallow-clone every body node
		for(Node* n : callee) {
			if(!isBodyNode(callee, n) || mapped(n))
				continue;
			Node* c = cloneShell(caller, n);
			if(!c)
				return false;
			mapNode(n, c);
			if(CallNode* cc = dyn_cast<CallNode>(c))
				worklist.push_back(cc);
		}
		return true;
	}

	// wire each clone's inputs through the map
	void InlinePass::wireClones(Function& callee) {
		for(Node* n : callee) {
			if(!isBodyNode(callee, n) || isStartProj(callee, n))
				continue; // seeded with an external value; nothing to wire
			Node* clone = mapped(n);
			if(!clone)
				continue;
			for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
				clone->setInput(i, resolve(n->getInput(i)));
		}
	}

	// collect the callee's mapped return triples and merge them
	InlinePass::Merged InlinePass::mergeReturns(Function& caller, const Function& callee) {
		ctrls.clear();
		mems.clear();
		vals.clear();
		StopNode* stop = callee.getStop();
		for(U32 i = 0, e = stop ? stop->getInputCount() : 0; i < e; ++i) {
			ReturnNode* r = dyn_cast<ReturnNode>(stop->getInput(i));
			if(!r)
				continue;
			ctrls.push_back(resolve(r->getControl()));
			mems.push_back(resolve(r->getMemory()));
			if(r->hasValue())
				vals.push_back(resolve(r->getValue()));
		}
		if(ctrls.empty())
			return {nullptr, nullptr, nullptr};
		if(ctrls.size() == 1)
			return {ctrls[0], mems[0], vals.empty() ? nullptr : vals[0]};
		RegionNode* reg = caller.create<RegionNode>(caller.ctrlTy(), ctrls);
		mems.insert(mems.begin(), reg);
		Merged m{reg, caller.create<PhiNode>(caller.memTy(), mems), nullptr};
		if(callee.returnsValue() && vals.size() == ctrls.size()) {
			vals.insert(vals.begin(), reg);
			m.val = caller.create<PhiNode>(callee.getReturnType(), vals);
		}
		return m;
	}

	// redirect the call's projections onto the merged values, then drop the
	// projections and the call itself
	void InlinePass::replaceCall(Function& caller, CallNode* call, const Merged& m) {
		for(ProjNode* pn : usersOfType<ProjNode>(call)) {
			U32 idx = pn->getIndex();
			Node* repl = nullptr;
			if(idx == CallNode::controlProjIndex())
				repl = m.ctrl;
			else if(idx == CallNode::memoryProjIndex())
				repl = m.mem;
			else if(idx == CallNode::valueProjIndex())
				repl = m.val;
			if(repl)
				pn->replaceAllUsesWith(repl);
			caller.removeNode(pn);
		}
		caller.removeNode(call);
	}

	B32 InlinePass::inlineCallSite(Function& caller, CallNode* call, Function& callee) {
		if(!cloneBody(caller, call, callee))
			return false;
		wireClones(callee);
		Merged m = mergeReturns(caller, callee);
		if(!m.ctrl)
			return false; // callee never returns
		replaceCall(caller, call, m);
		return true;
	}

	B32 InlinePass::shouldInline(const Function& caller, CallNode* call, Function* callee) {
		if(!callee || callee == &caller)
			return false; // missing or directly recursive
		if(callee->size() > kInlineNodeBudget)
			return false;
		if(callee->getAttrs().noInline)
			return false; // opted out via __attribute__((noinline))
		if(graph->isCyclic(callee))
			return false; // participates in a recursive cycle
		if(!callee->hasReturn())
			return false;
		if(call->returnsValue() != callee->returnsValue())
			return false;
		if(call->returnsValue() &&
			 call->getType()->getTupleElement(CallNode::valueProjIndex()) != callee->getReturnType())
			return false;
		if(call->getArgCount() != callee->getParamCount())
			return false;
		for(U32 i = 0, e = call->getArgCount(); i < e; ++i)
			if(call->getArg(i)->getType() != callee->getParamType(i))
				return false;
		return true;
	}

	void InlinePass::beginModule(Module&, const CallGraph& g) { graph = &g; }

	U32 InlinePass::runOnFunction(Function& caller, const TargetInfo&) {
		U32 limit = caller.size() + kCallerGrowthBudget;
		worklist.clear();
		for(Node* n : caller)
			if(CallNode* c = dyn_cast<CallNode>(n))
				worklist.push_back(c);

		// clones of inlined calls append to the worklist, so one scan inlines transitively
		U32 count = 0;
		for(U32 i = 0; i < worklist.size() && count < kMaxInlinesPerFunction && caller.size() <= limit;
				++i) {
			CallNode* c = worklist[i];
			Function* callee = graph->lookup(c->getCallee());
			if(shouldInline(caller, c, callee) && inlineCallSite(caller, c, *callee))
				++count;
		}
		if(count)
			caller.eliminateDeadNodes();
		return count;
	}

	const C8* InlinePass::name() const { return "inline"; }
} // namespace rat
