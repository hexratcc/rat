#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/type.h"
#include "pass/opt/load_canon.h"
#include "target/target.h"

#include <ostream>

namespace rat {
	namespace detail {
		// kLane a Load or Op lane, kSink a run store or reduction step, 0 other nodes and kept lanes
		enum Role : U8 {
			kLane = 1,
			kSink = 2
		};
	} // namespace detail

	U8 SlpPackPass::typeBits(const Type* t) {
		if(!t || !(t->isInt() || t->isFloat() || t->isPtr()))
			return 0;
		if(t->isPtr())
			return Ptr;
		if(t->isFloat())
			return (t->getFloatWidth() == 32) * Float32 | (t->getFloatWidth() == 64) * Float64;
		U32 bits = t->getIntWidth();
		return (bits == 8) * Int8 | (bits == 16) * Int16 | (bits == 32) * Int32 | (bits == 64) * Int64;
	}

	U64 SlpPackPass::pairKey(U32 high, I64 low) { return (U64)high << 32 | (U32)low; }

	// the end of the 16-byte block at from, or end when it comes first
	I64 SlpPackPass::blockEnd(I64 from, I64 end) {
		return std::min(end, (from | (I64)(kVecBytes - 1)) + 1);
	}

	SlpPackPass::Context::Context(Function& fn, const TargetInfo& target, SlpGuards guards)
	: fn(fn),
		ptrBytes(target.getPointerSizeInBytes()),
		sse41(target.hasSse41()),
		guards(guards) {}

	Type* SlpPackPass::Context::laneType(const Node* n) {
		if(isCompareOpcode(n->getOpcode()))
			return fn.types().getInt(8 * n->getInput(0)->getType()->byteSize(ptrBytes));
		return n->getType();
	}

	U32 SlpPackPass::Context::guardedRunsIn(const Node* block) const {
		auto it = guardedRunsPerBlock.find(block);
		if(it == guardedRunsPerBlock.end())
			return 0;
		return it->second;
	}

	SlpPackPass::Slp::Slp(Function& fn, const TargetInfo& target, SlpStats& stats, SlpGuards guards)
	: cx(fn, target, guards),
		stats(stats),
		check(fn),
		emitter(cx) {}

	U32 SlpPackPass::Slp::run() {
		LoadCanonPass::Handoff h = LoadCanonPass::takeHandoff(cx.fn);
		cx.addrs = std::move(h.addrs);
		List<B32> packed; // per chain
		U32 changed = packChains(h.chains, packed);
		LoadCanonPass::unhoistLoads(h.hoists, packed);
		emitter.splitBlocks();
		cx.fn.eliminateDeadNodes(); // so every reduction root found is live
		changed += packReductions();
		cx.fn.eliminateDeadNodes();
		return changed;
	}

	B32 SlpPackPass::Slp::pack(const Seed& seed) {
		Plan plan(cx, seed);
		I32 margin = -1;
		if(plan.build())
			margin = profitMargin(plan, cx.guards);
		if(margin < 0) {
			++stats.rejected;
			return false;
		}
		check.setRoles(plan, true);
		// the first innerReadersDie is an early reject, kept lanes can only make it fail
		B32 ok = check.innerReadersDie(seed) && (I32)check.markKeptLanes(plan) <= margin &&
						 check.innerReadersDie(seed);
		if(ok)
			apply(plan);
		check.setRoles(plan, false);
		stats.rejected += !ok;
		return ok;
	}

	// the profit above kMinProfit per window, what kept lanes may cost
	I32 SlpPackPass::profitMargin(const Plan& plan, SlpGuards guards) {
		I32 windows = std::max(1, (I32)plan.seed.lowestStores.size()); // a reduction counts as one
		I32 profit = plan.seed.profit - kMinProfit * windows;
		for(const Vector& vec : plan.vectors)
			profit += vec.profit;
		if(!plan.guards.empty() && guards != SlpGuards::NoCost)
			profit -= (I32)plan.guards.size() * kGuardCheckCost + kGuardBranchCost;
		return profit;
	}

	SlpPackPass::Check::Check(const Function& fn)
	: fn(fn) {}

	void SlpPackPass::Check::setRoles(const Plan& plan, B32 on) {
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

	// the wide stores make no inner state, so every load of one must die
	B32 SlpPackPass::Check::innerReadersDie(const Seed& seed) const {
		for(U32 j = 0; j + 1 < seed.stores.size(); ++j)
			for(Node* u : seed.stores[j]->getUsers())
				if(isa<LoadNode>(u) && !laneDies(u))
					return false;
		return true;
	}

	B32 SlpPackPass::Check::laneDies(const Node* n) const {
		return roleOf(n) == detail::kLane && !usedOutside(n, false);
	}

	// a lane a gather, shift or outside user reads stays scalar with its lane inputs, returns count
	U32 SlpPackPass::Check::markKeptLanes(const Plan& plan) {
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

	// n has a user besides the plan's Op lanes and sinks. With copies a store of n's value is
	// inside even when it is no sink, so a Load lane only copied to memory is not kept
	B32 SlpPackPass::Check::usedOutside(const Node* n, B32 copies) const {
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

	U8 SlpPackPass::Check::roleOf(const Node* n) const {
		if(n->getId() >= roles.size())
			return 0;
		return roles[n->getId()];
	}

	SlpPackPass::SlpPackPass(SlpGuards guards)
	: guards(guards) {}

	const C8* SlpPackPass::name() const {
		if(guards == SlpGuards::NoCost)
			return "slp-noguardcost";
		if(guards == SlpGuards::Off)
			return "slp-noguards";
		return "slp";
	}

	void SlpPackPass::beginModule(Module&, const CallGraph&) {
		stats = SlpStats{};
		LoadCanonPass::handoff() = {}; // a handoff from an earlier pipeline stage is stale
	}

	void SlpPackPass::printStats(const Module& module, std::ostream& os) const {
		const SlpStats& s = stats;
		os << "slp[" << module.getName() << "]: windows " << s.windowsSeen << " packed "
			 << (s.packedUnguarded + s.packedGuarded) << " (static " << s.packedUnguarded << ", guarded "
			 << s.packedGuarded << " in " << s.guardedRuns << " runs, " << s.guardChecks
			 << " checks) reductions " << s.packedReduction << " rejected " << s.rejected << "\n";
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		return Slp(fn, target, stats, guards).run();
	}
} // namespace rat
