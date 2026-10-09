#include "analysis/call_graph.h"

#include "ir/module.h"
#include "ir/node.h"

namespace rat {
	void CallGraph::build(Module& m) {
		funcs.clear();
		byName.clear();
		Map<const Function*, U32> slot;
		for(Function* fn : m) {
			slot.emplace(fn, (U32)funcs.size());
			byName.emplace(fn->getName(), fn);
			funcs.push_back(fn);
		}
		callees.assign(funcs.size(), {});
		for(U32 i = 0; i < funcs.size(); ++i)
			for(Node* n : *funcs[i])
				if(CallNode* c = dyn_cast<CallNode>(n))
					if(Function* callee = lookup(c->getCallee()))
						callees[i].push_back(slot[callee]);
		findComponents();
	}

	Function* CallGraph::lookup(const String& name) const {
		auto it = byName.find(name);
		return it == byName.end() ? nullptr : it->second;
	}

	B32 CallGraph::isCyclic(const Function* fn) const { return cyclic.count(fn) != 0; }

	const List<Function*>& CallGraph::bottomUp() const { return order; }

	void CallGraph::popComponent(U32 v) {
		U32 first = (U32)stack.size() - 1;
		while(stack[first] != v)
			--first;
		B32 loops = stack.size() - first > 1;
		for(U32 w : callees[v])
			loops = loops || w == v;
		for(U32 i = first; i < stack.size(); ++i) {
			Function* fn = funcs[stack[i]];
			onStack[stack[i]] = 0;
			order.push_back(fn);
			if(loops)
				cyclic.insert(fn);
		}
		stack.resize(first);
	}

	void CallGraph::findComponents() {
		U32 n = (U32)funcs.size(), next = 0;
		index.assign(n, ~0u);
		low.assign(n, 0);
		onStack.assign(n, 0);
		cyclic.clear();
		order.clear();
		List<Pair<U32, U32>> dfs; // (function, next callee slot)
		for(U32 r = 0; r < n; ++r) {
			if(index[r] != ~0u)
				continue;
			dfs.push_back({r, 0});
			while(!dfs.empty()) {
				auto [v, e] = dfs.back();
				if(e == 0) {
					index[v] = low[v] = next++;
					stack.push_back(v);
					onStack[v] = 1;
				}
				if(e < callees[v].size()) {
					dfs.back().second = e + 1;
					U32 w = callees[v][e];
					if(index[w] == ~0u)
						dfs.push_back({w, 0});
					else if(onStack[w])
						low[v] = std::min(low[v], index[w]);
					continue;
				}
				dfs.pop_back();
				if(low[v] == index[v])
					popComponent(v);
				if(!dfs.empty())
					low[dfs.back().first] = std::min(low[dfs.back().first], low[v]);
			}
		}
	}
} // namespace rat
