#include "Pass/Opt/MemoryOpt.h"

#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
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

	U32 MemoryOptPass::forwardStores(const AliasAnalysis& aa,
																	 const Map<LoadNode*, Node*>& defs,
																	 const List<LoadNode*>& loads) {
		U32 removed = 0;
		for(LoadNode* l : loads) {
			if(!l->hasUsers())
				continue;
			U32 sz = aa.getAccessSize(l);
			StoreNode* s = dyn_cast<StoreNode>(defs.at(l));
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

	U32 MemoryOptPass::cseLoads(Function& fn,
															const AliasAnalysis& aa,
															const Map<LoadNode*, Node*>& defs,
															const List<LoadNode*>& loads) {
		Schedule sched(fn);

		auto dominates = [&](LoadNode* a, LoadNode* b) -> B32 {
			I32 ba = sched.blockOf(a), bb = sched.blockOf(b);
			if(ba < 0 || bb < 0)
				return false;
			if(ba == bb)
				return a->getId() < b->getId(); // same block, no aliasing store between
			return sched.dominates(ba, bb);
		};

		struct BucketKey {
			Node* def;
			AliasAnalysis::MustAliasKey addr;
			B32 operator==(const BucketKey& o) const { return def == o.def && addr == o.addr; }
		};

		struct BucketKeyHash {
			U64 operator()(const BucketKey& k) const {
				U64 h = AliasAnalysis::MustAliasKeyHash{}(k.addr);
				h ^= (U64) reinterpret_cast<std::uintptr_t>(k.def) + 0x9e3779b97f4a7c15ull + (h << 6) +
						 (h >> 2);
				return h;
			}
		};

		std::unordered_map<BucketKey, List<LoadNode*>, BucketKeyHash> buckets;

		for(LoadNode* l : loads) {
			if(!l->hasUsers())
				continue;
			AliasAnalysis::MustAliasKey key = aa.mustAliasKey(l);
			if(!key.valid())
				continue; // opaque address / unknown size: not provably CSE
			buckets[BucketKey{defs.at(l), key}].push_back(l);
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

	B32 MemoryOptPass::run(Module& module) {
		U32 changed = 0;
		for(Function* fn : module)
			changed += runOnFunction(*fn);
		return changed != 0;
	}

	U32 MemoryOptPass::runOnFunction(Function& fn) {
		fn.eliminateDeadNodes();

		AliasAnalysis aa(fn);

		List<LoadNode*> loads;
		for(Node* n : fn)
			if(LoadNode* l = dyn_cast<LoadNode>(n))
				loads.push_back(l);
		Map<LoadNode*, Node*> defs;
		defs.reserve(loads.size());
		for(LoadNode* l : loads)
			defs.emplace(l, l->hasUsers() ? effectiveDef(aa, l) : nullptr);

		U32 removed = forwardStores(aa, defs, loads);
		removed += cseLoads(fn, aa, defs, loads);
		if(removed)
			fn.eliminateDeadNodes();
		return removed;
	}
} // namespace rat
