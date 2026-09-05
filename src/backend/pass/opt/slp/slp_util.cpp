// generic IR plumbing, address group signatures and the structural hash

#include "pass/opt/slp/slp_util.h"

#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

#include <cstdlib>

namespace rat {
	using namespace slp;

	B32 slp::envFlag(const C8* name) { return std::getenv(name) != nullptr; }

	B32 slp::packableElem(const Type* t) {
		if(!t)
			return false;
		if(t->isInt())
			return t->getIntWidth() == 32 || t->getIntWidth() == 64;
		if(t->isFloat())
			return t->getFloatWidth() == 32 || t->getFloatWidth() == 64;
		return false;
	}

	B32 slp::packableBinary(Opcode op, const Type* t) {
		switch(op) {
		case Opcode::Add:
		case Opcode::Sub:
		case Opcode::And:
		case Opcode::Or:
		case Opcode::Xor:
			return t->isInt();
		case Opcode::Mul:
			return t->isInt() && t->getIntWidth() == 32; // pmulld, no packed i64 mul in sse
		case Opcode::FAdd:
		case Opcode::FSub:
		case Opcode::FMul:
		case Opcode::FDiv:
			return t->isFloat();
		default:
			return false;
		}
	}

	String slp::groupSig(const RefinedAddr& k) {
		String s = std::to_string(k.base->getId());
		for(const auto& [var, scale] : k.terms) {
			s += ',';
			s += std::to_string(var->getId());
			s += ':';
			s += std::to_string(scale);
		}
		return s;
	}

	U32 slp::laneCountFor(U32 esz) { return kVecBytes / esz; }

	// element widths with an SSE packed form (i32/f32 and i64/f64)
	B32 slp::supportedEsz(U32 esz) { return esz == 4 || esz == 8; }

	B32 slp::dataCone(const Node* root, U32 cap, List<const Node*>& out) {
		List<const Node*> work = {root};
		Set<const Node*> seen;
		while(!work.empty()) {
			const Node* c = work.back();
			work.pop_back();
			if(!c || !seen.insert(c).second)
				continue;
			if(seen.size() > cap)
				return false; // unbounded cone
			out.push_back(c);
			for(U32 i = 0, e = c->getInputCount(); i < e; ++i)
				if(const Node* in = c->getInput(i))
					if(in->getType() && in->getType()->isData())
						work.push_back(in);
		}
		return true;
	}

	// first load reached walking the bounded value cone, or null
	LoadNode* slp::firstLoadInCone(Node* root, U32 cap) {
		// a partial cone (dataCone hit the cap) is fine: callers use this only for
		// best-effort canonical ordering
		List<const Node*> cone;
		dataCone(root, cap, cone);
		for(const Node* c : cone)
			if(const LoadNode* l = dyn_cast<LoadNode>(c))
				return const_cast<LoadNode*>(l);
		return nullptr;
	}

	// true when every user path from seed reaches only runStores, hopping through
	// arithmetic nodes; false on a foreign use or a cone larger than cap
	B32 slp::coneEndsInStores(const Node* seed, const Set<const Node*>& runStores, U32 cap) {
		List<const Node*> work = {seed};
		Set<const Node*> seen;
		while(!work.empty()) {
			const Node* c = work.back();
			work.pop_back();
			if(!seen.insert(c).second)
				continue;
			if(seen.size() > cap)
				return false;
			for(Node* u : c->getUsers()) {
				if(runStores.count(u))
					continue;
				if(isArithmeticOpcode(u->getOpcode()))
					work.push_back(u);
				else
					return false;
			}
		}
		return true;
	}

	B32 slp::usesValue(const Node* u, const Node* x) {
		for(U32 i = 0, e = u->getInputCount(); i < e; ++i)
			if(u->getInput(i) == x)
				return true;
		return false;
	}

	void slp::rewriteInput(Node* u, const Node* from, Node* to) {
		for(U32 t = 0, e = u->getInputCount(); t < e; ++t)
			if(u->getInput(t) == from)
				u->setInput(t, to);
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
			for(const auto& [var, scale] : ra.terms)
				h = mix(mix(h, var->getId()), (U64)scale);
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
