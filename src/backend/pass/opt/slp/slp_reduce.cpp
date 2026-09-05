// reduction vectorization

#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	using namespace slp;

	namespace detail {
		// refined address of the first leaf load in a reduction term, for canonical ordering
		B32 leafKey(Node* term, U32 esz, RefinedAddr& out) {
			LoadNode* l = firstLoadInCone(term, 64);
			if(!l)
				return false;
			out = refineAddr(l->getPointer(), esz);
			return out.valid();
		}
	} // namespace detail

	U32 slp::Slp::packReduction(BinaryNode* root) {
		Type* t = root->getType();
		U32 esz = t->byteSize(ptrBytes);
		U32 w = laneCountFor(esz);
		Opcode addOp = root->getOpcode();

		List<Node*> terms = flattenAddChain(root, addOp, t);
		U32 n = (U32)terms.size();
		if(n < 2 * w || n % w != 0)
			return 0;
		++stats.windowsSeen;

		// canonical term order: sort by the first leaf load's refined address so
		// grouping is robust against source-level reassociation
		struct Keyed {
			B32 operator<(const Keyed& o) const {
				if(sig != o.sig)
					return sig < o.sig;
				return c < o.c;
			}

			String sig;
			I64 c;
			Node* term;
		};
		List<Keyed> keyed;
		for(Node* term : terms) {
			RefinedAddr k;
			if(!detail::leafKey(term, esz, k))
				break;
			keyed.push_back({groupSig(k), k.constant, term});
		}
		if(keyed.size() == terms.size()) {
			std::stable_sort(keyed.begin(), keyed.end());
			for(U32 i = 0; i < n; ++i)
				terms[i] = keyed[i].term;
		}

		// no window - every packed load must read one shared pre-state
		Packer packer(*this, nullptr, nullptr);
		U32 k = n / w;
		List<Node*> vecs;
		for(U32 g = 0; g < k; ++g) {
			List<Node*> lanes(terms.begin() + g * w, terms.begin() + (g + 1) * w);
			Node* v = packer.packTuple(lanes, t, 0);
			if(!v) {
				++stats.rejectedTree;
				return 0;
			}
			vecs.push_back(v);
		}

		// horizontal-sum cost: 2 shuffle+add rounds for 4 lanes, 1 for 2 lanes
		I32 hsumOps = 3;
		if(w == 4)
			hsumOps = 5;
		packer.profit += (I32)n - 1;					 // scalar add chain removed
		packer.profit -= (I32)k - 1 + hsumOps; // vector combine + horizontal finish
		if(packer.profit < kMinProfit || packer.interior == 0) {
			++stats.rejectedProfit;
			return 0;
		}

		Type* vecTy = fn.types().getVec(t, w);
		Node* acc = vecs[0];
		for(U32 g = 1; g < k; ++g)
			acc = fn.create<BinaryNode>(addOp, vecTy, acc, vecs[g]);
		// log2 shuffle+add finish, result in every lane
		Node* s1 = fn.create<ShuffleNode>(vecTy, acc, (U8)0x4e); // swap 64-bit halves
		acc = fn.create<BinaryNode>(addOp, vecTy, acc, s1);
		if(w == 4) {
			Node* s2 = fn.create<ShuffleNode>(vecTy, acc, (U8)0xb1); // swap 32-bit pairs
			acc = fn.create<BinaryNode>(addOp, vecTy, acc, s2);
		}
		Node* res = fn.create<ExtractNode>(t, acc, 0);
		root->replaceAllUsesWith(res);
		++stats.packedReduction;
		return 1;
	}

	U32 slp::Slp::packReductions() {
		List<BinaryNode*> roots;
		for(Node* n : fn) {
			BinaryNode* b = dyn_cast<BinaryNode>(n);
			if(!b)
				continue;
			Opcode op = b->getOpcode();
			if(op != Opcode::Add && !(op == Opcode::FAdd && fpReduceEnabled()))
				continue;
			Type* t = b->getType();
			if(!t || !packableElem(t) || t->isInt() != (op == Opcode::Add))
				continue;
			B32 isRoot = true;
			for(Node* u : b->getUsers())
				if(u->getOpcode() == op && u->getType() == t)
					isRoot = false;
			if(isRoot)
				roots.push_back(b);
		}
		U32 changed = 0;
		for(BinaryNode* root : roots)
			changed += packReduction(root);
		return changed;
	}

	// flatten an add-reduction tree through single-use interior adds, back to source order
	List<Node*> slp::flattenAddChain(BinaryNode* root, Opcode addOp, Type* t) {
		List<Node*> terms;
		List<Node*> work = {root};
		while(!work.empty()) {
			Node* n = work.back();
			work.pop_back();
			BinaryNode* b = dyn_cast<BinaryNode>(n);
			// an interior add of the same type with a single user is part of the chain
			B32 sameAdd = b && b->getOpcode() == addOp && b->getType() == t;
			B32 singleUse = n == root || n->getUsers().size() == 1;
			B32 interior = sameAdd && singleUse;
			if(interior && terms.size() + work.size() < 64) {
				work.push_back(b->getLHS());
				work.push_back(b->getRHS());
			} else {
				terms.push_back(n);
			}
		}
		std::reverse(terms.begin(), terms.end());
		return terms;
	}
} // namespace rat
