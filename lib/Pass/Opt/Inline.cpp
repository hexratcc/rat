#include "Pass/Opt/Inline.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	B32 InlinePass::isStartProj(const Function& callee, Node* n) {
		ProjNode* p = dyn_cast<ProjNode>(n);
		return p && p->getProducer() == callee.getStart();
	}

	Node* InlinePass::incomingForStartProj(CallNode* call, U32 startProjIdx) {
		if (startProjIdx == StartNode::controlProjIndex())
			return call->getControl();
		if (startProjIdx == StartNode::memoryProjIndex())
			return call->getMemory();
		U32 a = startProjIdx - StartNode::paramProjIndex(0);
		return a < call->getArgCount() ? call->getArg(a) : nullptr;
	}

	B32 InlinePass::inlineCallSite(Function& caller, CallNode* call, Function& callee) {
		Map<Node*, Node*> map;
		for (ProjNode* p : usersOfType<ProjNode>(callee.getStart())) {
			Node* incoming = incomingForStartProj(call, p->getIndex());
			if (!incoming)
				return false;
			map[p] = incoming;
		}

		// shallow-clone every body node
		for (Node* n : callee) {
			if (n == callee.getStart() || n == callee.getStop())
				continue;
			if (n->getOpcode() == Opcode::Return)
				continue;
			if (map.count(n))
				continue;
			Node* c = cloneShell(caller, n);
			if (!c)
				return false;
			map[n] = c;
		}

		auto resolve = [&](Node* n) -> Node* {
			auto it = map.find(n);
			return it == map.end() ? n : it->second;
		};

		// wire each clone's inputs through the map
		for (auto& kv : map) {
			Node* orig = kv.first;
			if (isStartProj(callee, orig))
				continue; // seeded with an external value; nothing to wire
			Node* clone = kv.second;
			for (U32 i = 0, e = orig->getInputCount(); i < e; ++i)
				clone->setInput(i, resolve(orig->getInput(i)));
		}

		// collect the callee's mapped return triples and merge them
		List<Node*> ctrls, mems, vals;
		for (Node* n : callee) {
			ReturnNode* r = dyn_cast<ReturnNode>(n);
			if (!r)
				continue;
			ctrls.push_back(resolve(r->getControl()));
			mems.push_back(resolve(r->getMemory()));
			if (r->hasValue())
				vals.push_back(resolve(r->getValue()));
		}
		if (ctrls.empty())
			return false; // callee never returns

		Node* mergedCtrl = nullptr;
		Node* mergedMem = nullptr;
		Node* mergedVal = nullptr;

		if (ctrls.size() == 1) {
			mergedCtrl = ctrls[0];
			mergedMem = mems[0];
			mergedVal = vals.empty() ? nullptr : vals[0];
		} else {
			RegionNode* reg = caller.create<RegionNode>(caller.ctrlTy(), ctrls);
			mergedCtrl = reg;
			List<Node*> memIns{reg};
			for (Node* m : mems)
				memIns.push_back(m);
			mergedMem = caller.create<PhiNode>(caller.memTy(), memIns);
			if (callee.returnsValue() && vals.size() == ctrls.size()) {
				List<Node*> valIns{reg};
				for (Node* v : vals)
					valIns.push_back(v);
				mergedVal = caller.create<PhiNode>(callee.getReturnType(), valIns);
			}
		}

		// redirect the call's projections onto the merged values, then drop the
		// projections and the call itself
		for (ProjNode* pn : usersOfType<ProjNode>(call)) {
			U32 idx = pn->getIndex();
			Node* repl = nullptr;
			if (idx == CallNode::controlProjIndex())
				repl = mergedCtrl;
			else if (idx == CallNode::memoryProjIndex())
				repl = mergedMem;
			else if (idx == CallNode::valueProjIndex())
				repl = mergedVal;
			if (repl)
				pn->replaceAllUsesWith(repl);
			caller.removeNode(pn);
		}
		caller.removeNode(call);
		return true;
	}

	B32 InlinePass::shouldInline(const Function& caller, CallNode* call, Function* callee) {
		if (!callee || callee == &caller)
			return false; // missing or directly recursive
		if (!callee->hasReturn())
			return false;
		if (call->returnsValue() != callee->returnsValue())
			return false;
		if (call->getArgCount() != callee->getParamCount())
			return false;
		return callee->size() <= kInlineNodeBudget;
	}

	U32 InlinePass::inlineInto(Function& caller, Module& m) {
		U32 count = 0;
		B32 changed = true;
		while (changed && count < kMaxInlinesPerFunction) {
			changed = false;
			List<CallNode*> calls;
			for (Node* n : caller)
				if (CallNode* c = dyn_cast<CallNode>(n))
					calls.push_back(c);
			for (CallNode* c : calls) {
				Function* callee = m.getFunction(c->getCallee());
				if (!shouldInline(caller, c, callee))
					continue;
				if (inlineCallSite(caller, c, *callee)) {
					++count;
					changed = true;
					break;
				}
			}
		}
		if (count)
			caller.eliminateDeadNodes();
		return count;
	}

	const C8* InlinePass::name() const { return "inline"; }

	B32 InlinePass::run(Module& module) {
		U32 count = 0;
		for (Function* fn : module)
			count += inlineInto(*fn, module);
		return count != 0;
	}
} // namespace rat
