// memory optimization: redundant-load elimination and store-to-load
// forwarding over the explicit memory-state edges of the SON,
// disambiguated by AliasAnalysis
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995 (memory as a value)
// - F. Chow, S. Chan, S.-M. Liu, R. Lo and M. Streich, "Effective
//   Representation of Aliases and Indirect Memory Operations in SSA Form",
//   Compiler Construction (CC), 1996

#include "Pass/Opt/MemoryOpt.h"

#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "Pass/Opt/AliasAnalysis.h"

namespace rat {
	namespace {
		Node* effectiveDef(const AliasAnalysis& aa, Node* mem, Node* addr,
											 U32 size) {
			while (mem->getOpcode() == Opcode::Store) {
				StoreNode* s = static_cast<StoreNode*>(mem);
				if (aa.alias(addr, size, s->getPointer(), aa.accessSize(s)) ==
						AliasResult::NoAlias) {
					mem = s->getMemory();
					continue;
				}
				break;
			}
			return mem;
		}
	} // namespace

	U32 optimizeMemory(Function& fn) {
		AliasAnalysis aa(fn);

		List<LoadNode*> loads;
		for (Node* n : fn)
			if (n->getOpcode() == Opcode::Load)
				loads.push_back(static_cast<LoadNode*>(n));

		U32 removed = 0;

		// store-to-load forwarding
		for (LoadNode* l : loads) {
			if (!l->hasUsers())
				continue;
			U32 sz = aa.accessSize(l);
			Node* def = effectiveDef(aa, l->getMemory(), l->getPointer(), sz);
			if (def->getOpcode() != Opcode::Store)
				continue;
			StoreNode* s = static_cast<StoreNode*>(def);
			if (aa.alias(l->getPointer(), sz, s->getPointer(), aa.accessSize(s)) ==
							AliasResult::MustAlias &&
					aa.accessSize(s) == sz && s->getValue()->getType() == l->getType()) {
				l->replaceAllUsesWith(s->getValue());
				++removed;
			}
		}

		// load-after-load CSE
		Schedule sched(fn);
		List<LoadNode*> live;
		for (LoadNode* l : loads)
			if (l->hasUsers())
				live.push_back(l);

		auto dominates = [&](LoadNode* a, LoadNode* b) -> B32 {
			I32 ba = sched.blockOf(a), bb = sched.blockOf(b);
			if (ba < 0 || bb < 0)
				return false;
			if (ba == bb)
				return a->getId() < b->getId(); // same block, no aliasing store between
			return sched.dominates(ba, bb);
		};

		for (U32 i = 0; i < (U32)live.size(); ++i) {
			LoadNode* a = live[i];
			if (!a->hasUsers())
				continue;
			U32 szA = aa.accessSize(a);
			Node* defA = effectiveDef(aa, a->getMemory(), a->getPointer(), szA);
			for (U32 j = 0; j < (U32)live.size(); ++j) {
				if (i == j)
					continue;
				LoadNode* b = live[j];
				if (!b->hasUsers() || b->getType() != a->getType())
					continue;
				U32 szB = aa.accessSize(b);
				if (szB != szA)
					continue;
				if (effectiveDef(aa, b->getMemory(), b->getPointer(), szB) != defA)
					continue;
				if (aa.alias(a->getPointer(), szA, b->getPointer(), szB) !=
						AliasResult::MustAlias)
					continue;
				if (dominates(a, b)) {
					b->replaceAllUsesWith(a);
					++removed;
				}
			}
		}

		if (removed)
			fn.eliminateDeadNodes();
		return removed;
	}

	const char* MemoryOptPass::name() const { return "memoryopt"; }

	B32 MemoryOptPass::run(Module& module) {
		U32 removed = 0;
		for (Function* fn : module)
			removed += optimizeMemory(*fn);
		return removed != 0;
	}
} // namespace rat
