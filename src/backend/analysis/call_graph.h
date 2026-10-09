// direct call graph of a module: its call cycles and a bottom-up function order (callees
// before callers), both from tarjan's strongly connected components

#ifndef RAT_ANALYSIS_CALL_GRAPH_H
#define RAT_ANALYSIS_CALL_GRAPH_H

#include "core.h"

namespace rat {
	struct Function;
	struct Module;

	struct CallGraph {
		void build(Module& m);

		Function* lookup(const String& name) const; // a function of the module, or null
		B32 isCyclic(const Function* fn) const;			// on a call cycle (or calls itself)
		const List<Function*>& bottomUp() const;		// every function, callees first
	private:
		void findComponents();
		void popComponent(U32 v);

		List<Function*> funcs;
		List<List<U32>> callees; // per function, indices into funcs
		Map<String, Function*> byName;
		Set<const Function*> cyclic;
		List<Function*> order;
		List<U32> index, low, stack; // tarjan state
		List<U8> onStack;
	};
} // namespace rat

#endif
