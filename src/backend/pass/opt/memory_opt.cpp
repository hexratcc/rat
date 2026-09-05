#include "pass/opt/memory_opt.h"

#include "codegen/schedule.h"
#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/node.h"
#include "target/target.h"

namespace rat {
	Node* MemoryOptPass::effectiveDef(const AliasAnalysis& aa, Node* mem, Node* addr, U32 size) {
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
		Schedule sched(fn, Schedule::Mode::Loads);

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
			buckets[BucketKey{defs[l->getId()], l->getType(), key}].push_back(l);
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

	MemoryOptPass::ChainScan
	MemoryOptPass::scanPhiSide(const AliasAnalysis& aa, PhiNode* m, U32 side, LoadNode* l) {
		ChainScan scan{nullptr, false, false};
		U32 size = aa.getAccessSize(l);
		Node* mem = m->getValue(side);
		for(U32 steps = 0;; ++steps) {
			if(mem == m) {
				scan.loops = true;
				return scan;
			}
			StoreNode* s = dyn_cast<StoreNode>(mem);
			if(!s)
				return scan;
			if(steps >= kMaxStoreWalk) {
				scan.clobbered = true;
				return scan;
			}
			// stores older than the found one are overwritten
			if(!scan.store) {
				AliasResult r = aa.alias(l->getPointer(), size, s->getPointer(), aa.getAccessSize(s));
				if(r == AliasResult::MustAlias && aa.getAccessSize(s) == size &&
					 s->getValue()->getType() == l->getType())
					scan.store = s;
				else if(r != AliasResult::NoAlias)
					scan.clobbered = true;
			}
			mem = s->getMemory();
		}
	}

	// a load whose def is a loop memory phi reads either the value stored on
	// the previous iteration or the last store before the loop, forward both
	// through a data phi and the load (not the store) becomes dead
	U32 MemoryOptPass::forwardLoopCarried(Function& fn, const AliasAnalysis& aa) {
		U32 removed = 0;
		for(LoadNode* l : loads) {
			if(!l->hasUsers())
				continue;
			PhiNode* m = dyn_cast<PhiNode>(defs[l->getId()]);
			if(!m || !m->getType()->isMemory() || m->getValueCount() != 2)
				continue;
			if(!m->getRegion()->isLoopHeader())
				continue;
			ChainScan a = scanPhiSide(aa, m, 0, l);
			ChainScan b = scanPhiSide(aa, m, 1, l);
			if(a.loops == b.loops) // need exactly one backedge
				continue;
			const ChainScan& entry = a.loops ? b : a;
			const ChainScan& back = a.loops ? a : b;
			if(entry.clobbered || back.clobbered || !entry.store)
				continue;
			Node* init = entry.store->getValue();
			Node* replacement;
			if(!back.store || back.store->getValue() == l) {
				replacement = init;
			} else {
				List<Node*> values{init, init};
				values[a.loops ? 0 : 1] = back.store->getValue();
				replacement = fn.phi(l->getType(), m->getRegion(), values);
			}
			l->replaceAllUsesWith(replacement);
			++removed;
		}
		return removed;
	}

	const C8* MemoryOptPass::name() const { return "memoryopt"; }

	U32 MemoryOptPass::runOnFunction(Function& fn, const TargetInfo& target) {
		fn.eliminateDeadNodes();

		AliasAnalysis aa(target.getPointerSizeInBytes());

		loads.clear();
		for(Node* n : fn)
			if(LoadNode* l = dyn_cast<LoadNode>(n))
				loads.push_back(l);
		defs.assign(fn.idBound(), nullptr);
		for(LoadNode* l : loads)
			if(l->hasUsers())
				defs[l->getId()] = effectiveDef(aa, l->getMemory(), l->getPointer(), aa.getAccessSize(l));

		U32 removed = 0;
		removed += forwardStores(aa);
		removed += cseLoads(fn, aa);
		removed += forwardLoopCarried(fn, aa);
		if(removed)
			fn.eliminateDeadNodes();
		return removed;
	}
} // namespace rat
