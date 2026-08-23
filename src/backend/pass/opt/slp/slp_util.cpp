// generic IR plumbing

#include "pass/opt/slp/slp_util.h"

#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "pass/opt/alias_analysis.h"

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

	B32 slp::identifiedBase(const Node* n) { return n && AliasAnalysis::isIdentified(n); }

	B32 slp::isI64(const Node* n) {
		return n->getType() && n->getType()->isInt() && n->getType()->getIntWidth() == 64;
	}

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
		// best-effort canonical ordering / anchoring
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

	U32 slp::laneCountFor(U32 esz) { return kVecBytes / esz; }

	// element widths with an SSE packed form (i32/f32 and i64/f64)
	B32 slp::supportedEsz(U32 esz) { return esz == 4 || esz == 8; }
} // namespace rat
