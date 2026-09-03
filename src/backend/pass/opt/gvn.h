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
			size_t operator()(const GVNKey& k) const {
				U64 h = 1469598103934665603ull;
				auto mix = [&](U64 v) {
					h ^= v;
					h *= 1099511628211ull;
				};
				mix(k.op);
				mix(k.type);
				mix((U64)k.payload);
				mix(k.in0);
				mix(((U64)k.in1) << 32);
				if(k.sym)
					mix(std::hash<String>{}(*k.sym));
				return (size_t)h;
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
