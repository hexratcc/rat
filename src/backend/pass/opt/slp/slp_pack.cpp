#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/type.h"
#include "pass/opt/load_canon.h"
#include "pass/opt/slp/slp_pack_detail.h"
#include "target/target.h"

#include <cstdlib>
#include <iostream>

namespace rat {
	namespace slp {
		namespace detail {
			// kLane a Load or Op lane, kSink a run store or reduction step, 0 other nodes and kept lanes
			enum Role : U8 { kLane = 1, kSink = 2 };
		} // namespace detail

		Slp::Slp(Function& fn, const TargetInfo& target, SlpStats& stats)
		: fn(fn),
			ptrBytes(target.getPointerSizeInBytes()),
			sse41(target.hasSse41()),
			stats(stats) {}

		U32 Slp::run() {
			Handoff h = takeHandoff(fn);
			addrs = std::move(h.addrs);
			List<B32> packed; // per chain
			U32 changed = packChains(h.chains, packed);
			unhoistLoads(h.hoists, packed);
			splitBlocks();
			fn.eliminateDeadNodes(); // so every reduction root found is live
			changed += packReductions();
			fn.eliminateDeadNodes();
			return changed;
		}

		B32 Slp::pack(const Seed& seed) {
			Plan plan(*this, seed);
			I32 margin = -1;
			if(plan.build())
				margin = profitMargin(plan);
			B32 ok = margin >= 0;
			if(ok) {
				setRoles(plan, true);
				// the first innerReadersDie is a cheap early reject, as kept lanes can only make it fail
				ok = innerReadersDie(seed) && (I32)markKeptLanes(plan) <= margin && innerReadersDie(seed);
				if(ok)
					apply(plan);
				setRoles(plan, false);
			}
			stats.rejected += !ok;
			return ok;
		}

		Type* Slp::laneType(const Node* n) {
			if(isCompareOpcode(n->getOpcode()))
				return fn.types().getInt(8 * n->getInput(0)->getType()->byteSize(ptrBytes));
			return n->getType();
		}

		U8 typeBits(const Type* t) {
			if(!t || !(t->isInt() || t->isFloat() || t->isPtr()))
				return 0;
			if(t->isPtr())
				return kPtr;
			if(t->isFloat())
				return (t->getFloatWidth() == 32) * kF32 | (t->getFloatWidth() == 64) * kF64;
			U32 bits = t->getIntWidth();
			return (bits == 8) * kI8 | (bits == 16) * kI16 | (bits == 32) * kI32 | (bits == 64) * kI64;
		}

		U64 pairKey(U32 high, I64 low) { return (U64)high << 32 | (U32)low; }

		// the end of the 16-byte block at from, or end when it comes first
		I64 blockEnd(I64 from, I64 end) { return std::min(end, (from | (I64)(kVecBytes - 1)) + 1); }

		I32 profitMargin(const Plan& plan) {
			I32 profit = plan.seed.profit;
			for(const Vector& vec : plan.vectors)
				profit += vec.profit;
			static const B32 noGuardCost = std::getenv("RAT_SLP_NO_GUARD_COST") != nullptr;
			if(!plan.guards.empty() && !noGuardCost)
				profit -= (I32)plan.guards.size() * kGuardCheckCost + kGuardBranchCost;
			return profit;
		}

		// the state before a run, valid until commitRun clears the stores
		Node* memIn(const Seed& seed) { return seed.stores[0]->getMemory(); }

		B32 isInner(const Seed& seed, const Node* state) {
			for(U32 j = 0; j + 1 < seed.stores.size(); ++j)
				if(seed.stores[j] == state)
					return true;
			return false;
		}

		// the set of a run of set members, their only user, null for a run of a chain
		MemMergeNode* setOf(const Seed& seed) {
			return dyn_cast<MemMergeNode>(seed.stores[0]->getUsers()[0]);
		}

		void Slp::setRoles(const Plan& plan, B32 on) {
			if(roles.size() < fn.idBound())
				roles.resize(2 * fn.idBound(), 0); // room for the nodes later seeds make
			U8 laneRole = (U8)(detail::kLane * on);
			U8 sinkRole = (U8)(detail::kSink * on);
			for(const Vector& vec : plan.vectors)
				if(vec.kind != VectorKind::Gather)
					for(Node* n : vec.lanes)
						roles[n->getId()] = laneRole;
			for(Node* n : plan.seed.stores)
				roles[n->getId()] = sinkRole;
			for(Node* n : plan.seed.steps)
				roles[n->getId()] = sinkRole;
		}

		U8 Slp::roleOf(const Node* n) const {
			if(n->getId() >= roles.size())
				return 0;
			return roles[n->getId()];
		}

		// a lane a scalar user needs stays scalar with its lane inputs, so it drops its role
		U32 Slp::markKeptLanes(const Plan& plan) {
			SmallList<const Node*, 64> work;
			for(const Vector& vec : plan.vectors) {
				for(Node* n : vec.lanes)
					if(vec.kind == VectorKind::Gather || usedOutside(n, vec.kind == VectorKind::Load))
						work.push_back(n);
				if(vec.shiftAmount)
					work.push_back(vec.shiftAmount);
			}
			U32 kept = 0;
			while(!work.empty()) {
				const Node* n = work.back();
				work.pop_back();
				if(roleOf(n) != detail::kLane)
					continue;
				roles[n->getId()] = 0;
				++kept;
				for(U32 i = 0; i < n->getInputCount(); ++i)
					work.push_back(n->getInput(i));
			}
			return kept;
		}

		// a user of n besides the lanes and sinks of the plan, with copies a store of n's value counts
		// as inside even when it is no sink, so a Load lane only copied to memory is not kept
		B32 Slp::usedOutside(const Node* n, B32 copies) const {
			for(U32 i = 0; i < n->getUsers().size(); ++i) {
				const Node* user = n->getUsers()[i];
				// a load lane reads n as its scalar pointer
				B32 inside = roleOf(user) != 0 && !isa<LoadNode>(user);
				if(isa<StoreNode>(user)) // input 3 is the value
					inside = n->getUserSlot(i) == 3 && (copies || roleOf(user) == detail::kSink);
				if(!inside)
					return true;
			}
			return false;
		}

		B32 Slp::laneDies(const Node* n) const {
			return roleOf(n) == detail::kLane && !usedOutside(n, false);
		}

		// every load of a state between the run's stores dies, as the wide stores make no such state
		B32 Slp::innerReadersDie(const Seed& seed) const {
			for(U32 j = 0; j + 1 < seed.stores.size(); ++j)
				for(Node* u : seed.stores[j]->getUsers())
					if(isa<LoadNode>(u) && !laneDies(u))
						return false;
			return true;
		}
	} // namespace slp

	const C8* SlpPackPass::name() const { return "slp"; }

	void SlpPackPass::beginModule(Module&, const CallGraph&) {
		stats = SlpStats{};
		slp::handoff() = {}; // a handoff from an earlier pipeline stage is stale
	}

	void SlpPackPass::endModule(Module& module) {
		const SlpStats& s = stats;
		if(std::getenv("RAT_SLP_STATS"))
			std::cerr << "slp[" << module.getName() << "]: windows " << s.windowsSeen << " packed "
								<< (s.packedUnguarded + s.packedGuarded) << " (static " << s.packedUnguarded
								<< ", guarded " << s.packedGuarded << " in " << s.guardedRuns << " runs, "
								<< s.guardChecks << " checks) reductions " << s.packedReduction << " rejected "
								<< s.rejected << "\n";
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		return slp::Slp(fn, target, stats).run();
	}
} // namespace rat
