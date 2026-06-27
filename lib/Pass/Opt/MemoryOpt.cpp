#include "Pass/Opt/MemoryOpt.h"

#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Node.h"
#include "Pass/Opt/AliasAnalysis.h"

namespace rat {
	Node* MemoryOptPass::effectiveDef(const AliasAnalysis& aa, Node* mem, Node* addr, U32 size) {
		while(StoreNode* s = dyn_cast<StoreNode>(mem)) {
			if(aa.alias(addr, size, s->getPointer(), aa.getAccessSize(s)) == AliasResult::NoAlias) {
				mem = s->getMemory();
				continue;
			}
			break;
		}
		return mem;
	}

	Node* MemoryOptPass::effectiveDef(const AliasAnalysis& aa, LoadNode* l) {
		return effectiveDef(aa, l->getMemory(), l->getPointer(), aa.getAccessSize(l));
	}

	U32 MemoryOptPass::forwardStores(const AliasAnalysis& aa, const List<LoadNode*>& loads) {
		U32 removed = 0;
		for(LoadNode* l : loads) {
			if(!l->hasUsers())
				continue;
			U32 sz = aa.getAccessSize(l);
			StoreNode* s = dyn_cast<StoreNode>(effectiveDef(aa, l));
			if(!s)
				continue;
			if(aa.alias(l->getPointer(), sz, s->getPointer(), aa.getAccessSize(s)) ==
						 AliasResult::MustAlias &&
				 aa.getAccessSize(s) == sz && s->getValue()->getType() == l->getType()) {
				l->replaceAllUsesWith(s->getValue());
				++removed;
			}
		}
		return removed;
	}

	U32 MemoryOptPass::cseLoads(Function& fn, const AliasAnalysis& aa, const List<LoadNode*>& loads) {
		Schedule sched(fn);
		List<LoadNode*> live;
		for(LoadNode* l : loads)
			if(l->hasUsers())
				live.push_back(l);

		auto dominates = [&](LoadNode* a, LoadNode* b) -> B32 {
			I32 ba = sched.blockOf(a), bb = sched.blockOf(b);
			if(ba < 0 || bb < 0)
				return false;
			if(ba == bb)
				return a->getId() < b->getId(); // same block, no aliasing store between
			return sched.dominates(ba, bb);
		};

		Map<Node*, List<LoadNode*>> byDef;
		for(LoadNode* l : live)
			byDef[effectiveDef(aa, l)].push_back(l);

		U32 removed = 0;
		for(auto& kv : byDef) {
			List<LoadNode*>& group = kv.second;
			for(U32 i = 0, e = (U32)group.size(); i < e; ++i) {
				LoadNode* a = group[i];
				if(!a->hasUsers())
					continue;
				U32 szA = aa.getAccessSize(a);
				for(U32 j = 0; j < e; ++j) {
					if(i == j)
						continue;
					LoadNode* b = group[j];
					if(!b->hasUsers() || b->getType() != a->getType())
						continue;
					U32 szB = aa.getAccessSize(b);
					if(szB != szA)
						continue;
					if(aa.alias(a->getPointer(), szA, b->getPointer(), szB) != AliasResult::MustAlias)
						continue;
					if(dominates(a, b)) {
						b->replaceAllUsesWith(a);
						++removed;
					}
				}
			}
		}
		return removed;
	}

	const C8* MemoryOptPass::name() const { return "memoryopt"; }

	U32 MemoryOptPass::runOnFunction(Function& fn) {
		fn.eliminateDeadNodes();

		AliasAnalysis aa(fn);

		List<LoadNode*> loads;
		for(Node* n : fn)
			if(LoadNode* l = dyn_cast<LoadNode>(n))
				loads.push_back(l);

		U32 removed = forwardStores(aa, loads);
		removed += cseLoads(fn, aa, loads);
		if(removed)
			fn.eliminateDeadNodes();
		return removed;
	}
} // namespace rat
