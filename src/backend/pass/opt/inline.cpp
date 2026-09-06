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
		auto it = byName.find(name);
		return it == byName.end() ? nullptr : it->second;
	}

	// rebuild fn's callee row
	B32 InlinePass::refreshCallees(Function& fn, Info& info) {
		List<Info*> row;
		for(Node* n : fn) {
			CallNode* c = dyn_cast<CallNode>(n);
			if(!c)
				continue;
			if(Function* next = lookup(c->getCallee()))
				row.push_back(&infos[next]);
		}
		std::sort(row.begin(), row.end());
		row.erase(std::unique(row.begin(), row.end()), row.end());
		info.version = fn.getVersion();
		if(row == info.callees)
			return false;
		info.callees.swap(row);
		return true; // edges changed
	}

	// forget the functions dfe removed, and the edges into them
	void InlinePass::dropDeadRows(Module& m) {
		Set<const Function*> alive;
		for(Function* fn : m)
			alive.insert(fn);
		for(auto& [fn, info] : infos) {
			List<Info*>& row = info.callees;
			U32 w = 0;
			for(Info* callee : row)
				if(alive.count(callee->fn))
					row[w++] = callee;
			row.resize(w);
		}
		for(auto it = infos.begin(); it != infos.end();) {
			if(alive.count(it->first))
				++it;
			else
				it = infos.erase(it);
		}
	}

	void InlinePass::forgetCycles() {
		for(auto& [fn, info] : infos)
			info.cyclic = -1;
	}

	// refresh the rows of functions mutated since their row was built
	void InlinePass::syncCallGraph(Module& m) {
		if(module != &m) {
			infos.clear();
			byName.clear();
			module = &m;
		}
		if(byName.size() != m.size()) {
			byName.clear();
			for(Function* fn : m) {
				byName.emplace(fn->getName(), fn);
				infos[fn].fn = fn;
			}
			if(infos.size() > m.size())
				dropDeadRows(m);
		}
		B32 changed = false;
		for(Function* fn : m) {
			Info& info = infos[fn];
			if(info.version != fn->getVersion() && refreshCallees(*fn, info))
				changed = true;
		}
		if(changed)
			forgetCycles();
	}

	B32 InlinePass::reaches(Info* from, Info* target) {
		if(from->visit == visitCur)
			return false;
		from->visit = visitCur;
		for(Info* next : from->callees)
			if(next == target || reaches(next, target))
				return true;
		return false;
	}

	B32 InlinePass::isCyclic(Function* fn) {
		Info& info = infos[fn];
		if(info.cyclic < 0) {
			++visitCur;
			info.cyclic = reaches(&info, &info);
		}
		return info.cyclic;
	}

	// caller version folded with its direct callees' versions
	U64 InlinePass::quietStamp(const Function& caller, const Info& info) const {
		U64 stamp = 14695981039346656037ull ^ caller.getVersion();
		for(Info* callee : info.callees)
			stamp = stamp * 1099511628211ull + callee->fn->getVersion();
		return stamp;
	}

	B32 InlinePass::shouldInline(const Function& caller, CallNode* call, Function* callee) {
		if(!callee || callee == &caller)
			return false; // missing or directly recursive
		if(callee->getAttrs().noInline)
			return false; // opted out via __attribute__((noinline))
		if(isCyclic(callee))
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
		return callee->size() <= kInlineNodeBudget;
	}

	B32 InlinePass::run(Module& module, const TargetInfo& target) {
		syncCallGraph(module);
		return FunctionPass::run(module, target);
	}

	U32 InlinePass::runOnFunction(Function& caller, const TargetInfo&) {
		if(module != &caller.getModule())
			syncCallGraph(caller.getModule());
		Info& info = infos[&caller];
		U64 stamp = quietStamp(caller, info);
		if(info.quietAt == stamp)
			return 0;
		if(!info.firstSize)
			info.firstSize = caller.size();
		U32 limit = info.firstSize + kCallerGrowthBudget;

		worklist.clear();
		for(Node* n : caller)
			if(CallNode* c = dyn_cast<CallNode>(n))
				worklist.push_back(c);

		U32 count = 0;
		B32 changed = true;
		while(changed && count < kMaxInlinesPerFunction && caller.size() <= limit) {
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
		if(!count) {
			info.quietAt = stamp;
			return 0;
		}
		caller.eliminateDeadNodes();
		if(refreshCallees(caller, info))
			forgetCycles();
		return count;
	}

	const C8* InlinePass::name() const { return "inline"; }
} // namespace rat
