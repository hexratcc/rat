// global value numbering: hash-cons congruent nodes so equal computations
// share a single node
//
// references:
// - B. Alpern, M. Wegman and F. K. Zadeck, "Detecting Equality of Variables
//   in Programs", POPL, 1988
// - C. Click, "Global Code Motion / Global Value Numbering", PLDI, 1995

#ifndef RAT_PASS_OPT_GVN_H
#define RAT_PASS_OPT_GVN_H

#include "core.h"
#include "hash.h"
#include "pass/pass.h"

namespace rat {
	struct Function;
	struct Node;

	namespace detail {
		struct GVNKey {
			U32 op = 0;
			U32 type = 0;
			I64 payload = 0;						 // constant value
			const String* sym = nullptr; // global symbol (interned in the node)
			U32 in0 = ~0u, in1 = ~0u;

			B32 operator==(const GVNKey& o) const {
				return op == o.op && type == o.type && payload == o.payload && in0 == o.in0 &&
							 in1 == o.in1 && (sym == o.sym || (sym && o.sym && *sym == *o.sym));
			}
		};

		struct GVNKeyHash {
			U64 operator()(const GVNKey& k) const {
				U64 h = kFnvBasis;
				hashMix(h, k.op);
				hashMix(h, k.type);
				hashMix(h, (U64)k.payload);
				hashMix(h, k.in0);
				hashMix(h, ((U64)k.in1) << 32);
				if(k.sym)
					hashMix(h, std::hash<String>{}(*k.sym));
				return (U64)h;
			}
		};

		B32 makeKey(Node* n, GVNKey& k);
	} // namespace detail

	struct GVNPass : FunctionPass {
		const C8* name() const override;
		U32 runOnFunction(Function& fn, const TargetInfo& target) override;
	private:
		static B32 isPureValue(Node* n);
	private:
		struct Slot {
			detail::GVNKey key;
			Node* val = nullptr;
		};
		List<Slot> slots;
	};
} // namespace rat

#endif
