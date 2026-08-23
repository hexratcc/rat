// structural hashing & address group signatures

#include "pass/opt/slp/slp_pack.h"

#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	using namespace slp;

	String slp::groupSig(const RefinedAddr& k) {
		String s = std::to_string(k.base->getId());
		for(const auto& t : k.terms) {
			s += ',';
			s += std::to_string(t.first->getId());
			s += ':';
			s += std::to_string(t.second);
		}
		return s;
	}

	U64 slp::ShapeHash::mix(U64 h, U64 v) {
		h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
		return h;
	}

	U64 slp::ShapeHash::operator()(const Node* n) { return shape(n, kDepth); }

	U64 slp::ShapeHash::shape(const Node* n, U32 depth) {
		if(!n)
			return 0x51ab;
		if(depth == kDepth) {
			if(auto it = memo.find(n); it != memo.end())
				return it->second;
		}
		U64 h = mix((U64)n->getOpcode() << 8, n->getType()->getUid());
		if(const LoadNode* l = dyn_cast<LoadNode>(n)) {
			RefinedAddr ra = refineAddr(const_cast<LoadNode*>(l)->getPointer(), 1);
			U64 baseId = 0;
			if(ra.base)
				baseId = ra.base->getId();
			h = mix(h, baseId);
			for(const auto& t : ra.terms)
				h = mix(mix(h, t.first->getId()), (U64)t.second);
		}
		if(depth && isArithmeticOpcode(n->getOpcode())) {
			U32 e = n->getInputCount();
			if(n->isCommutative() && e == 2) {
				// order-independent combine so commutative operands hash the same either way
				U64 a = shape(n->getInput(0), depth - 1);
				U64 b = shape(n->getInput(1), depth - 1);
				if(a < b)
					h = mix(h, mix(a, b));
				else
					h = mix(h, mix(b, a));
			} else {
				for(U32 i = 0; i < e; ++i)
					h = mix(h, shape(n->getInput(i), depth - 1));
			}
		}
		if(depth == kDepth)
			memo.emplace(n, h);
		return h;
	}
} // namespace rat
