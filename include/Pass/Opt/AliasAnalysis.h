// alias analysis: decide whether two memory accesses may, must, or never
// refer to overlapping storage, used to drive MemoryOpt
//
// references:
// - F. Chow, S. Chan, S.-M. Liu, R. Lo and M. Streich, "Effective
//   Representation of Aliases and Indirect Memory Operations in SSA Form",
//   Compiler Construction (CC), 1996

#ifndef RAT_PASS_OPT_ALIASANALYSIS_H
#define RAT_PASS_OPT_ALIASANALYSIS_H

#include "Core.h"

namespace rat {
	struct Function;
	struct Node;

	enum class AliasResult { NoAlias, MayAlias, MustAlias };

	struct AliasAnalysis {
		explicit AliasAnalysis(const Function& fn);

		AliasResult alias(Node* addrA, U32 sizeA, Node* addrB, U32 sizeB) const;
		AliasResult alias(Node* accessA, Node* accessB) const;

		U32 getAccessSize(const Node* access) const;
	private:
		struct Address {
			Node* base;						// pointer
			I64 constant;					// base offset
			List<Node*> symbolic; // non-constant addends
		};

		Address decompose(Node* addr) const;

		static B32 isIdentified(const Node* n);
		static B32 distinctObjects(const Node* a, const Node* b);
	private:
		const Function& fn;
	};
} // namespace rat

#endif
