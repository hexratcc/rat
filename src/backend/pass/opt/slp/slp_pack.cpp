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

		B32 guardsDisabled() {
			static const B32 v = std::getenv("RAT_SLP_NO_GUARDS") != nullptr;
			return v;
		}

		B32 guardCostDisabled() {
			static const B32 v = std::getenv("RAT_SLP_NO_GUARD_COST") != nullptr;
			return v;
		}

		B32 statsEnabled() {
			static const B32 v = std::getenv("RAT_SLP_STATS") != nullptr;
			return v;
		}

		Slp::Slp(Function& fn, const TargetInfo& target, SlpStats& stats)
		: fn(fn),
			ptrBytes(target.getPointerSizeInBytes()),
			sse41(target.hasSse41()),
			stats(stats),
			addrs(AliasAnalysis::restrictFacts(fn), fn.idBound()) {}

		// the disjoint stretches become sets, which processSet searches by lookup, then the sets become
		// chains again
		U32 Slp::run() {
			List<Piece> pieces = mergeStretches();
			U32 changed = 0;
			for(const Piece& p : pieces) {
				if(p.set)
					changed += processSet(p.set);
				else
					changed += processChain(p.chain);
			}
			lowerSets(pieces);
			changed += processSeams();
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
				ok = innerReadersDie(seed) && (I32)keepLanes(plan) <= margin && innerReadersDie(seed);
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

		Node* memoryOperand(const Node* n) {
			B32 access = isa<LoadNode>(n) || isa<StoreNode>(n) || isa<CallNode>(n) || isa<AsmNode>(n);
			if(access || isStackOpcode(n->getOpcode()))
				return n->getInput(1);
			return nullptr;
		}

		void replaceUses(Node* from, Node* to) {
			for(U32 k = from->getUsers().size(); k-- > 0;)
				from->getUsers()[k]->setInput(from->getUserSlot(k), to);
		}

		I32 gatherCost(const Lanes& lanes) {
			if(oneScalar(lanes))
				return 1;
			I32 vars = 0;
			B32 base = isa<ConstantNode>(lanes[0]); // needs a pool load
			for(Node* n : lanes) {
				if(const ConstantNode* c = dyn_cast<ConstantNode>(n))
					base |= c->getValue() != 0;
				else
					++vars;
			}
			B32 anyConstant = vars < (I32)lanes.size();
			return vars + (base || !anyConstant);
		}

		B32 isInner(const Seed& seed, const Node* m) {
			for(U32 j = 0; j + 1 < seed.stores.size(); ++j)
				if(seed.stores[j] == m)
					return true;
			return false;
		}

		B32 oneScalar(const Lanes& lanes) {
			for(Node* n : lanes)
				if(n != lanes[0])
					return false;
			return !isa<ConstantNode>(lanes[0]);
		}

		I32 profitMargin(const Seed& seed, const Plan& plan) {
			I32 profit = seed.profit;
			for(const Record& r : plan.records)
				profit += r.profit;
			U32 guards = (U32)plan.guards.size();
			if(guards && !guardCostDisabled())
				profit -= (I32)guards * kGuardCheckCost + kGuardBranchCost;
			if(guards > kMaxGuards)
				return -1;
			return profit - kMinProfit * (I32)seed.windows;
		}

		// set the roles of the Load and Op lanes, run stores and reduction steps, or clear them
		void Slp::setRoles(const Seed& seed, const Plan& plan, B32 on) {
			if(roles.size() < fn.idBound())
				roles.resize(2 * fn.idBound(), 0); // room for the nodes later seeds make
			U8 load = 0, op = 0, sink = 0;
			if(on) {
				load = kLoadLane;
				op = kOpLane;
				sink = kSink;
			}
			for(const Record& r : plan.records) {
				if(r.kind == Kind::Gather)
					continue;
				U8 role = op;
				if(r.kind == Kind::Load)
					role = load;
				for(Node* n : r.lanes)
					roles[n->getId()] = role;
			}
			for(Node* n : seed.stores)
				roles[n->getId()] = sink;
			for(Node* n : seed.steps)
				roles[n->getId()] = sink;
		}

		U8 Slp::roleOf(const Node* n) const {
			if(n->getId() >= roles.size())
				return 0;
			return roles[n->getId()];
		}

		U32 Slp::keepLanes(const Plan& plan) {
			SmallList<const Node*, 64> work;
			for(const Record& r : plan.records) {
				for(Node* n : r.lanes)
					if(r.kind == Kind::Gather || usedOutside(n, r.kind == Kind::Load))
						work.push_back(n);
				if(r.amount)
					work.push_back(r.amount);
			}
			U32 kept = 0;
			while(!work.empty()) {
				const Node* n = work.back();
				work.pop_back();
				if(!(roleOf(n) & (kLoadLane | kOpLane)) || (roleOf(n) & kKept))
					continue;
				roles[n->getId()] |= kKept;
				++kept;
				for(U32 i = 0; i < n->getInputCount(); ++i)
					work.push_back(n->getInput(i));
			}
			return kept;
		}

		B32 Slp::usedOutside(const Node* n, B32 copies) const {
			for(U32 k = 0; k < n->getUsers().size(); ++k) {
				const Node* u = n->getUsers()[k];
				B32 inside = roleOf(u) & (kOpLane | kSink);
				if(isa<StoreNode>(u)) // input 3 is the value
					inside = n->getUserSlot(k) == 3 && (copies || (roleOf(u) & kSink));
				if(!inside)
					return true;
			}
			return false;
		}

		B32 Slp::dies(const Node* n) const {
			return (roleOf(n) & (kLoadLane | kOpLane)) && !(roleOf(n) & kKept) && !usedOutside(n, false);
		}

		// every load of a state between the run's stores dies, as the wide stores make no such state
		B32 Slp::innerReadersDie(const Seed& seed) const {
			for(U32 j = 0; j + 1 < seed.stores.size(); ++j)
				for(Node* u : seed.stores[j]->getUsers())
					if(isa<LoadNode>(u) && !dies(u))
						return false;
			return true;
		}
	} // namespace slp

	const C8* SlpPackPass::name() const { return "slp"; }

	void SlpPackPass::beginModule(Module&, const CallGraph&) { stats = SlpStats{}; }

	void SlpPackPass::endModule(Module& module) {
		const SlpStats& s = stats;
		if(slp::statsEnabled())
			std::cerr << "slp[" << module.getName() << "]: windows " << s.windowsSeen << " packed "
								<< (s.packedUnguarded + s.packedGuarded) << " (static " << s.packedUnguarded
								<< ", guarded " << s.packedGuarded << " in " << s.guardedRuns << " runs, "
								<< s.guardPairs << " checks) reductions " << s.packedReduction << " rejected "
								<< s.rejected << "\n";
	}

	U32 SlpPackPass::runOnFunction(Function& fn, const TargetInfo& target) {
		return slp::Slp(fn, target, stats).run();
	}
} // namespace rat
