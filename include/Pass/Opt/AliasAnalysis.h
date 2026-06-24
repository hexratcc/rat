#ifndef RAT_PASS_OPT_ALIASANALYSIS_H
#define RAT_PASS_OPT_ALIASANALYSIS_H

#include "Core.h"

namespace rat {
	struct Function;
	struct Node;

	enum class AliasResult { NoAlias, MayAlias, MustAlias };

	const C8* toString(AliasResult r);

	struct AliasAnalysis {
		explicit AliasAnalysis(const Function& fn);

		AliasResult alias(Node* addrA, U32 sizeA, Node* addrB, U32 sizeB) const;
		AliasResult alias(Node* accessA, Node* accessB) const;

		U32 accessSize(const Node* access) const;

	private:
		struct AddrInfo {
			Node* base;						// the underlying (non-decomposable) pointer
			I64 constant;					// constant byte offset from base
			List<Node*> symbolic; // non-constant addends (sorted by id)
		};

		const Function& fn;
		AddrInfo decompose(Node* addr) const;
	};
} // namespace rat

#endif
