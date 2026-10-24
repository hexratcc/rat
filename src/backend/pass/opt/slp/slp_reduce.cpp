#include "pass/opt/slp/slp_pack_detail.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {
		U32 Slp::packReductions() {
			// roots first, as a pack adds nodes to fn
			List<Node*> roots;
			for(Node* n : cx.fn)
				if(isReductionRoot(n))
					roots.push_back(n);
			U32 changed = 0;
			for(Node* root : roots)
				changed += packReduction(root);
			return changed;
		}

		// a reducible op with an input of its op and no user of its op and type
		B32 Slp::isReductionRoot(const Node* n) {
			Opcode op = n->getOpcode();
			if(op != Opcode::Add && op != Opcode::Mul && (op < Opcode::And || op > Opcode::Xor))
				return false;
			U8 typeClass = typeBits(n->getType());
			// a Mul packs only as SSE4.1 pmulld on i32
			if(!(typeClass & kI32I64) || (op == Opcode::Mul && (typeClass != kI32 || !cx.sse41)))
				return false;
			B32 root = false;
			for(U32 i = 0; i < n->getInputCount(); ++i)
				root |= n->getInput(i)->getOpcode() == op;
			for(Node* u : n->getUsers())
				root &= u->getOpcode() != op || u->getType() != n->getType();
			return root;
		}

		U32 Slp::packReduction(Node* root) {
			Seed seed;
			seed.root = root;
			List<Node*> terms = flattenReduction(root, seed.steps);
			U32 termCount = (U32)terms.size();
			if(termCount < kMinTerms)
				return 0;
			++stats.windowsSeen;
			U32 laneCount = kVecBytes / root->getType()->byteSize(cx.ptrBytes);
			U32 vectorCount = termCount / laneCount;
			seed.tuples.resize(vectorCount);
			for(U32 i = 0; i < vectorCount; ++i)
				seed.tuples[i].assign(terms.data() + i * laneCount, terms.data() + (i + 1) * laneCount);
			seed.scalarTerms.assign(terms.begin() + vectorCount * laneCount, terms.end());
			I32 finishOps = 2 * (I32)(laneCount / 2) + 1; // shuffle and op per round, then the extract
			// termCount - 1 scalar ops become vectorCount - 1 vector ops, finishOps and the scalar terms
			seed.profit =
					(I32)termCount - 1 - (I32)seed.scalarTerms.size() - ((I32)vectorCount - 1 + finishOps);
			return pack(seed);
		}

		// root's terms left to right, below its steps: single-use ops of its opcode and type
		List<Node*> flattenReduction(Node* root, List<Node*>& steps) {
			List<Node*> terms;
			List<Node*> todo = {root};
			while(!todo.empty()) {
				Node* n = todo.back();
				todo.pop_back();
				B32 isStep = n->getOpcode() == root->getOpcode() && n->getType() == root->getType() &&
										 (n == root || n->getUsers().size() == 1);
				// a step replaces n by its two inputs, which can each end as a term
				if(!isStep || terms.size() + todo.size() + 2 > kMaxTerms) {
					terms.push_back(n);
					continue;
				}
				steps.push_back(n);
				todo.push_back(n->getInput(0));
				todo.push_back(n->getInput(1));
			}
			std::reverse(terms.begin(), terms.end());
			return terms;
		}

		// combine the vectors by the op, fold the lanes by shuffles, then the other terms as scalars
		void Emitter::commitReduction(const Plan& plan) {
			const Seed& seed = plan.seed;
			Opcode op = seed.root->getOpcode();
			Node* acc = plan.tupleNode(0);
			Type* vecTy = acc->getType();
			for(U32 i = 1; i < plan.tupleVectors.size(); ++i)
				acc = cx.fn.create<BinaryNode>(op, vecTy, acc, plan.tupleNode(i));
			U32 laneCount = vecTy->getVecLanes();
			// finish shuffles by [laneCount / 4][round], nibble i is the source of lane i
			constexpr U16 kFinishSel[2][2] = {{0x01}, {0x1032, 0x2301}};
			for(U32 round = 0; round < laneCount / 2; ++round) {
				U16 sel = kFinishSel[laneCount / 4][round];
				Node* swapped = cx.fn.create<ShuffleNode>(vecTy, acc, acc, sel);
				acc = cx.fn.create<BinaryNode>(op, vecTy, acc, swapped);
			}
			Node* result = cx.fn.create<ExtractNode>(seed.root->getType(), acc, 0);
			for(Node* term : seed.scalarTerms)
				result = cx.fn.create<BinaryNode>(op, seed.root->getType(), result, term);
			seed.root->replaceAllUsesWith(result);
		}
	} // namespace slp
} // namespace rat
