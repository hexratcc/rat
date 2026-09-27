#include "pass/opt/dead_func_elim.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"

namespace rat {
	const C8* DeadFuncElimPass::name() const { return "dfe"; }

	const List<String>& DeadFuncElimPass::referencesOf(Function& fn, Set<String>& scratch) {
		auto it = refCache.find(&fn);
		if(it == refCache.end() || it->second.first != fn.getVersion()) {
			scratch.clear();
			for(Node* n : fn) {
				if(CallNode* c = dyn_cast<CallNode>(n)) {
					if(!c->isIndirect())
						scratch.insert(c->getCallee());
				} else if(GlobalNode* g = dyn_cast<GlobalNode>(n)) {
					scratch.insert(g->getSymbol());
				}
			}
			List<String> refs(scratch.begin(), scratch.end());
			Pair<U64, List<String>> entry{fn.getVersion(), std::move(refs)};
			it = refCache.insert_or_assign(&fn, std::move(entry)).first;
		}
		return it->second.second;
	}

	B32 DeadFuncElimPass::run(Module& module, const TargetInfo&) {
		List<Function*> funcs;
		for(Function* fn : module)
			funcs.push_back(fn);
		const U32 n = (U32)funcs.size();

		List<const List<String>*> outgoing(n);
		Map<String, U32> refCount;
		Map<String, List<U32>> byName;
		Set<String> scratch;
		for(U32 i = 0; i < n; ++i) {
			outgoing[i] = &referencesOf(*funcs[i], scratch);
			for(const String& s : *outgoing[i])
				++refCount[s];
			byName[funcs[i]->getName()].push_back(i);
		}
		for(const Global* g : module.globals())
			for(const Reloc& r : g->getRelocs())
				++refCount[r.symbol];

		List<B32> removed(n, false);
		List<U32> work;
		for(U32 i = 0; i < n; ++i)
			if(funcs[i]->getAttrs().isInternal() && !refCount.count(funcs[i]->getName()))
				work.push_back(i);

		B32 changed = false;
		while(!work.empty()) {
			U32 i = work.back();
			work.pop_back();
			if(removed[i])
				continue;
			removed[i] = true;
			module.removeFunction(funcs[i]);
			changed = true;

			for(const String& s : *outgoing[i]) {
				auto rc = refCount.find(s);
				if(rc == refCount.end() || rc->second == 0 || --rc->second != 0)
					continue;
				auto bn = byName.find(s);
				if(bn == byName.end())
					continue;
				for(U32 j : bn->second)
					if(!removed[j] && funcs[j]->getAttrs().isInternal())
						work.push_back(j);
			}
			refCache.erase(funcs[i]);
		}
		return changed;
	}
} // namespace rat
