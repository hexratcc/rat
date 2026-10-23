#include "pass/opt/slp/slp_pack.h"

#include "analysis/alias_analysis.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/type.h"
#include "target/target.h"

#include <cstdlib>
#include <iostream>

namespace rat {
	namespace slp {

		Handoff& handoff() {
			thread_local Handoff h;
			return h;
		}

		Handoff takeHandoff(Function& fn) {
			Handoff& h = handoff();
			if(h.fn == &fn && h.version == fn.getVersion()) {
				h.fn = nullptr; // taken
				return std::move(h);
			}
			return freshHandoff(fn);
		}

		Handoff freshHandoff(Function& fn) {
			U64 restrictParams = AliasAnalysis::restrictFacts(fn);
			return {&fn, 0, collectChains(fn), AddrCache(restrictParams, fn.idBound()), {}};
		}

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
			Planner planner(*this, seed);
			const Plan& plan = planner.plan;
			I32 margin = -1;
			if(planner.planSeed())
				margin = profitMargin(seed, plan);
			B32 ok = margin >= 0;
			if(ok) {
				setRoles(seed, plan, true);
				// the first innerReadersDie is a cheap early reject, as kept lanes can only make it fail
				ok = innerReadersDie(seed) && (I32)markKeptLanes(plan) <= margin && innerReadersDie(seed);
				if(ok)
					apply(seed, plan);
				setRoles(seed, plan, false);
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

		// as SSE4.1 lowers it, a movd or insert per variable lane and a pool vector when lane 0 or a
		// nonzero lane is a constant
		I32 gatherCost(const Lanes& lanes, B32 reduction) {
			if(isSplat(lanes))
				return 1;
			I32 variables = 0;
			B32 poolLoad = isa<ConstantNode>(lanes[0]);
			for(Node* n : lanes) {
				if(const ConstantNode* c = dyn_cast<ConstantNode>(n))
					poolLoad |= c->getValue() != 0;
				else
					++variables;
			}
			// a reduction of lanes that are all scalars waits for their moves into xmm
			B32 moveLatency = reduction && variables == (I32)lanes.size();
			return variables + poolLoad + moveLatency;
		}

		B32 isSplat(const Lanes& lanes) {
			for(Node* n : lanes)
				if(n != lanes[0])
					return false;
			return !isa<ConstantNode>(lanes[0]);
		}

		I32 profitMargin(const Seed& seed, const Plan& plan) {
			I32 profit = seed.profit;
			for(const Vector& vec : plan.vectors)
				profit += vec.profit;
			static const B32 noGuardCost = std::getenv("RAT_SLP_NO_GUARD_COST") != nullptr;
			if(!plan.guards.empty() && !noGuardCost)
				profit -= (I32)plan.guards.size() * kGuardCheckCost + kGuardBranchCost;
			return profit - kMinProfit * (I32)seed.windows;
		}

		B32 isInner(const Seed& seed, const Node* state) {
			for(U32 j = 0; j + 1 < seed.stores.size(); ++j)
				if(seed.stores[j] == state)
					return true;
			return false;
		}

		void Slp::setRoles(const Seed& seed, const Plan& plan, B32 on) {
			if(roles.size() < fn.idBound())
				roles.resize(2 * fn.idBound(), 0); // room for the nodes later seeds make
			U8 laneRole = (U8)(kLane * on);
			U8 sinkRole = (U8)(kSink * on);
			for(const Vector& vec : plan.vectors)
				if(vec.kind != VectorKind::Gather)
					for(Node* n : vec.lanes)
						roles[n->getId()] = laneRole;
			for(Node* n : seed.stores)
				roles[n->getId()] = sinkRole;
			for(Node* n : seed.steps)
				roles[n->getId()] = sinkRole;
		}

		U8 Slp::roleOf(const Node* n) const {
			if(n->getId() >= roles.size())
				return 0;
			return roles[n->getId()];
		}

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
				if(!(roleOf(n) & kLane) || (roleOf(n) & kKept))
					continue;
				roles[n->getId()] |= kKept;
				++kept;
				for(U32 i = 0; i < n->getInputCount(); ++i)
					work.push_back(n->getInput(i));
			}
			return kept;
		}

		B32 Slp::usedOutside(const Node* n, B32 copies) const {
			for(U32 i = 0; i < n->getUsers().size(); ++i) {
				const Node* user = n->getUsers()[i];
				// a load lane reads n as its scalar pointer
				B32 inside = (roleOf(user) & (kLane | kSink)) && !isa<LoadNode>(user);
				if(isa<StoreNode>(user)) // input 3 is the value
					inside = n->getUserSlot(i) == 3 && (copies || (roleOf(user) & kSink));
				if(!inside)
					return true;
			}
			return false;
		}

		B32 Slp::laneDies(const Node* n) const {
			return (roleOf(n) & kLane) && !(roleOf(n) & kKept) && !usedOutside(n, false);
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
