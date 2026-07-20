// memory optimization: redundant-load elimination and store-to-load
// forwarding over the explicit memory-state edges of the SON,
// disambiguated by AliasAnalysis
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995 (memory as a value)
// - F. Chow, S. Chan, S.-M. Liu, R. Lo and M. Streich, "Effective
//   Representation of Aliases and Indirect Memory Operations in SSA Form",
//   Compiler Construction (CC), 1996

#ifndef RAT_PASS_OPT_MEMORYOPT_H
#define RAT_PASS_OPT_MEMORYOPT_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct AliasAnalysis;
	struct Function;
	struct LoadNode;
	struct Module;
	struct Node;

	struct MemoryOptPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		static U32 forwardStores(const AliasAnalysis& aa,
														 const Map<LoadNode*, Node*>& defs,
														 const List<LoadNode*>& loads);
		static U32 cseLoads(Function& fn,
												const AliasAnalysis& aa,
												const Map<LoadNode*, Node*>& defs,
												const List<LoadNode*>& loads);

		// skip back over stores that provably do not alias [addr, addr+size)
		static Node* effectiveDef(const AliasAnalysis& aa, Node* mem, Node* addr, U32 size);
		static Node* effectiveDef(const AliasAnalysis& aa, LoadNode* l);
	};
} // namespace rat

#endif
