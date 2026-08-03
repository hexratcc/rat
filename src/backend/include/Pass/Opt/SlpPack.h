#ifndef RAT_PASS_OPT_SLPPACK_H
#define RAT_PASS_OPT_SLPPACK_H

#include "Core.h"

#include "IR/Opcode.h"
#include "Pass/Pass.h"

namespace rat {
	struct Node;
	struct StoreNode;
	struct LoadNode;
	struct Type;
	struct AliasAnalysis;

	struct SlpPackPass : FunctionPass {
		struct RefinedAddr {
			Node* base = nullptr;
			I64 constant = 0;
			List<std::pair<const Node*, I64>> terms; // (var, scale), sorted by id
			U32 size = 0;														 // access bytes

			B32 valid() const { return base != nullptr && size != 0; }
			B32 sameGroup(const RefinedAddr& o) const {
				return base == o.base && size == o.size && terms == o.terms;
			}
		};

		const C8* name() const override { return "slp"; }
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;

		// impl
		static B32 packableElem(const Type* t);
		static B32 packableBinary(Opcode op, const Type* t);
		static B32 identifiedBase(const Node* n);
		static B32 isI64(const Node* n);
		static void refineTerm32(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
		static void refineTerm(const Node* n, I64 scale, RefinedAddr& out, U32 depth);
		static RefinedAddr refineAddr(Node* addr, U32 accessBytes);
		static String groupSig(const RefinedAddr& k);
		static B32 provablyDisjoint(const AliasAnalysis& aa,
																Node* pa,
																const RefinedAddr& ka,
																U32 sza,
																Node* pb,
																const RefinedAddr& kb,
																U32 szb);
	};
} // namespace rat

#endif
