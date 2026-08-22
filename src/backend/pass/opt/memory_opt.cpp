#include "pass/opt/memory_opt.h"

#include "codegen/schedule.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "pass/opt/alias_analysis.h"
#include "target/target.h"

namespace rat {
	Node* MemoryOptPass::effectiveDef(const AliasAnalysis& aa, Node* mem, Node* addr, U32 size) {
		// cap the chain walk: unbounded it is O(loads * chain) and blows up on
		// huge by-value struct copies; hitting the cap only forgoes forwarding,
		// still sound
		constexpr U32 kMaxStoreWalk = 512;
		for(U32 steps = 0; steps < kMaxStoreWalk; ++steps) {
			StoreNode* s = dyn_cast<StoreNode>(mem);
			if(!s)
				break;
			if(aa.alias(addr, size, s->getPointer(), aa.getAccessSize(s)) != AliasResult::NoAlias)
				break;
			mem = s->getMemory();
		}
		return mem;
	}

	Node* MemoryOptPass::effectiveDef(const AliasAnalysis& aa, LoadNode* l) {
		return effectiveDef(aa, l->getMemory(), l->getPointer(), aa.getAccessSize(l));
	}

	U32 MemoryOptPass::forwardStores(const AliasAnalysis& aa) {
		U32 removed = 0;
		for(LoadNode* l : loads) {
			if(!l->hasUsers())
				continue;
			U32 sz = aa.getAccessSize(l);
			StoreNode* s = dyn_cast<StoreNode>(defs[l->getId()]);
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

	U32 MemoryOptPass::cseLoads(Function& fn, const AliasAnalysis& aa) {
		Schedule sched(fn);

		auto dominates = [&](LoadNode* a, LoadNode* b) -> B32 {
			I32 ba = sched.blockOf(a), bb = sched.blockOf(b);
			if(ba < 0 || bb < 0)
				return false;
			if(ba == bb)
				return a->getId() < b->getId(); // same block, no aliasing store between
			return sched.dominates(ba, bb);
		};

		buckets.clear();
		for(LoadNode* l : loads) {
			if(!l->hasUsers())
				continue;
			AliasAnalysis::MustAliasKey key = aa.mustAliasKey(l);
			if(!key.valid())
				continue; // opaque address / unknown size: not provably CSE
			buckets[BucketKey{defs[l->getId()], key}].push_back(l);
		}

		U32 removed = 0;
		for(auto& kv : buckets) {
			List<LoadNode*>& group = kv.second;
			if(group.size() < 2)
				continue;
			std::sort(group.begin(), group.end(), [&](LoadNode* a, LoadNode* b) {
				I32 ba = sched.blockOf(a), bb = sched.blockOf(b);
				I32 da = ba < 0 ? -1 : sched.block(ba).domDepth;
				I32 db = bb < 0 ? -1 : sched.block(bb).domDepth;
				if(da != db)
					return da < db;
				return a->getId() < b->getId();
			});
			for(U32 i = 0, e = (U32)group.size(); i < e; ++i) {
				LoadNode* b = group[i];
				if(!b->hasUsers())
					continue;
				for(U32 j = 0; j < i; ++j) {
					LoadNode* a = group[j];
					if(!a->hasUsers())
						continue;
					if(dominates(a, b)) {
						b->replaceAllUsesWith(a);
						++removed;
						break;
					}
				}
			}
		}
		return removed;
	}

	const C8* MemoryOptPass::name() const { return "memoryopt"; }

	U32 MemoryOptPass::runOnFunction(Function& fn, const TargetInfo& target) {
		fn.eliminateDeadNodes();

		AliasAnalysis aa(fn, target.getPointerSizeInBytes());

		loads.clear();
		for(Node* n : fn)
			if(LoadNode* l = dyn_cast<LoadNode>(n))
				loads.push_back(l);
		defs.assign(fn.idBound(), nullptr);
		for(LoadNode* l : loads)
			if(l->hasUsers())
				defs[l->getId()] = effectiveDef(aa, l);

		U32 removed = forwardStores(aa);
		removed += cseLoads(fn, aa);
		if(removed)
			fn.eliminateDeadNodes();
		return removed;
	}
} // namespace rat
