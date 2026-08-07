#include "pass/opt/inline.h"

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

	B32 InlinePass::inlineCallSite(Function& caller,
																 CallNode* call,
																 Function& callee,
																 List<CallNode*>& newCalls) {
		cloneMap.clear();
		auto put = [&](Node* key, Node* val) {
			U32 id = key->getId();
			if(id >= cloneMap.size())
				cloneMap.resize(id + 1, nullptr);
			cloneMap[id] = val;
		};
		auto mapped = [&](Node* key) -> Node* {
			U32 id = key->getId();
			return id < cloneMap.size() ? cloneMap[id] : nullptr;
		};

		for(ProjNode* p : usersOfType<ProjNode>(callee.getStart())) {
			Node* incoming = incomingForStartProj(call, p->getIndex());
			if(!incoming)
				return false;
			put(p, incoming);
		}

		// shallow-clone every body node
		for(Node* n : callee) {
			if(n == callee.getStart() || n == callee.getStop())
				continue;
			if(n->getOpcode() == Opcode::Return)
				continue;
			if(mapped(n))
				continue;
			Node* c = cloneShell(caller, n);
			if(!c)
				return false;
			put(n, c);
			if(CallNode* cc = dyn_cast<CallNode>(c))
				newCalls.push_back(cc);
		}

		auto resolve = [&](Node* n) -> Node* {
			if(!n)
				return n;
			Node* m = mapped(n);
			return m ? m : n;
		};

		// wire each clone's inputs through the map
		for(Node* n : callee) {
			if(n == callee.getStart() || n == callee.getStop())
				continue;
			if(n->getOpcode() == Opcode::Return)
				continue;
			if(isStartProj(callee, n))
				continue; // seeded with an external value; nothing to wire
			Node* clone = mapped(n);
			if(!clone)
				continue;
			for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
				clone->setInput(i, resolve(n->getInput(i)));
		}
		// collect the callee's mapped return triples and merge them
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
			return false; // callee never returns

		Node* mergedCtrl = nullptr;
		Node* mergedMem = nullptr;
		Node* mergedVal = nullptr;

		if(ctrls.size() == 1) {
			mergedCtrl = ctrls[0];
			mergedMem = mems[0];
			mergedVal = vals.empty() ? nullptr : vals[0];
		} else {
			RegionNode* reg = caller.create<RegionNode>(caller.ctrlTy(), ctrls);
			mergedCtrl = reg;
			List<Node*> memIns{reg};
			for(Node* m : mems)
				memIns.push_back(m);
			mergedMem = caller.create<PhiNode>(caller.memTy(), memIns);
			if(callee.returnsValue() && vals.size() == ctrls.size()) {
				List<Node*> valIns{reg};
				for(Node* v : vals)
					valIns.push_back(v);
				mergedVal = caller.create<PhiNode>(callee.getReturnType(), valIns);
			}
		}

		// redirect the call's projections onto the merged values, then drop the
		// projections and the call itself
		for(ProjNode* pn : usersOfType<ProjNode>(call)) {
			U32 idx = pn->getIndex();
			Node* repl = nullptr;
			if(idx == CallNode::controlProjIndex())
				repl = mergedCtrl;
			else if(idx == CallNode::memoryProjIndex())
				repl = mergedMem;
			else if(idx == CallNode::valueProjIndex())
				repl = mergedVal;
			if(repl)
				pn->replaceAllUsesWith(repl);
			caller.removeNode(pn);
		}
		caller.removeNode(call);
		return true;
	}

	Function* InlinePass::lookup(const String& name) const {
		auto it = graphByName.find(name);
		return it == graphByName.end() ? nullptr : graphFuncs[it->second];
	}

	void InlinePass::refreshCallees(Function& fn) {
		auto self = graphIndex.find(&fn);
		if(self == graphIndex.end())
			return;
		List<U32>& row = graphCallees[self->second];
		row.clear();
		++edgeStampCur;
		for(Node* n : fn) {
			CallNode* c = dyn_cast<CallNode>(n);
			if(!c)
				continue;
			Function* next = lookup(c->getCallee());
			if(!next)
				continue;
			auto ni = graphIndex.find(next);
			if(ni == graphIndex.end())
				continue;
			U32 j = ni->second;
			if(edgeStamp[j] == edgeStampCur)
				continue;
			edgeStamp[j] = edgeStampCur;
			row.push_back(j);
		}
	}

	void InlinePass::buildCallGraph(Module& m) {
		graphModule = &m;
		graphFuncs.clear();
		graphByName.clear();
		graphIndex.clear();
		for(Function* fn : m) {
			U32 i = (U32)graphFuncs.size();
			graphByName.emplace(fn->getName(), i);
			graphIndex[fn] = i;
			graphFuncs.push_back(fn);
		}
		graphCallees.assign(graphFuncs.size(), List<U32>());
		visitStamp.assign(graphFuncs.size(), 0);
		edgeStamp.assign(graphFuncs.size(), 0);
		visitStampCur = 0;
		edgeStampCur = 0;
		cyclicCache.clear();
		for(Function* fn : graphFuncs)
			refreshCallees(*fn);
	}

	B32 InlinePass::reachesIndex(U32 from, U32 target) {
		if(visitStamp[from] == visitStampCur)
			return false;
		visitStamp[from] = visitStampCur;
		for(U32 next : graphCallees[from])
			if(next == target || reachesIndex(next, target))
				return true;
		return false;
	}

	B32 InlinePass::isCyclic(Function* fn) {
		auto it = cyclicCache.find(fn);
		if(it != cyclicCache.end())
			return it->second;
		B32 cyc = false;
		auto gi = graphIndex.find(fn);
		if(gi != graphIndex.end()) {
			++visitStampCur;
			cyc = reachesIndex(gi->second, gi->second);
		}
		cyclicCache[fn] = cyc;
		return cyc;
	}

	B32 InlinePass::shouldInline(const Function& caller, CallNode* call, Function* callee) {
		if(!callee || callee == &caller)
			return false; // missing or directly recursive
		if(isCyclic(callee))
			return false; // participates in a recursive cycle
		if(!callee->hasReturn())
			return false;
		if(call->returnsValue() != callee->returnsValue())
			return false;
		if(call->getArgCount() != callee->getParamCount())
			return false;
		for(U32 i = 0, e = call->getArgCount(); i < e; ++i)
			if(call->getArg(i)->getType() != callee->getParamType(i))
				return false;
		return callee->size() <= kInlineNodeBudget;
	}

	B32 InlinePass::run(Module& module, const TargetInfo& target) {
		graphModule = nullptr;
		return FunctionPass::run(module, target);
	}

	U32 InlinePass::runOnFunction(Function& caller, const TargetInfo&) {
		Module& m = caller.getModule();
		if(graphModule != &m)
			buildCallGraph(m);

		U32 count = 0;
		U32 baseline = caller.size();

		worklist.clear();
		for(Node* n : caller)
			if(CallNode* c = dyn_cast<CallNode>(n))
				worklist.push_back(c);

		B32 changed = true;
		while(changed && count < kMaxInlinesPerFunction &&
					caller.size() <= baseline + kCallerGrowthBudget) {
			changed = false;
			for(U32 i = 0; i < worklist.size(); ++i) {
				CallNode* c = worklist[i];
				if(!c)
					continue;
				Function* callee = lookup(c->getCallee());
				if(!shouldInline(caller, c, callee)) {
					worklist[i] = nullptr;
					continue;
				}
				if(inlineCallSite(caller, c, *callee, worklist)) {
					worklist[i] = nullptr;
					++count;
					changed = true;
					break;
				}
			}
		}
		if(count) {
			caller.eliminateDeadNodes();
			refreshCallees(caller);
			cyclicCache.clear();
		}
		return count;
	}

	const C8* InlinePass::name() const { return "inline"; }
} // namespace rat
