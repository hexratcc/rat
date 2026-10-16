#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		Planner::Planner(Slp& s, const Seed& seed)
		: s(s),
			seed(seed) {
			plan.records.reserve(4 * seed.tuples.size());
		}

		// a record per seed tuple, false when a tuple has none, then the guards of the weak edges
		B32 Planner::planSeed() {
			for(const Lanes& t : seed.tuples) {
				budget += kMaxOps;
				U32 at = vectorOf(t);
				if(at == kNone)
					return false;
				plan.tuples.push_back(at);
			}
			for(const Record& r : plan.records)
				if(r.guard)
					addGuard(r);
			return true;
		}

		U32 Planner::vectorOf(const Lanes& lanes) {
			for(const Record& r : plan.records)
				if(r.lanes[0] == lanes[0] &&
					 std::equal(lanes.begin(), lanes.end(), r.lanes.begin(), r.lanes.end()))
					return (U32)(&r - plan.records.data());
			if(oneScalar(lanes))
				return gather(lanes);
			U32 at = loads(lanes);
			if(at == kNone)
				at = op(lanes);
			if(at == kNone)
				at = gather(lanes);
			return at;
		}

		// a Gather record: the scalars lane by lane, or a broadcast of one scalar
		U32 Planner::gather(const Lanes& lanes) {
			if(!(typeBits(lanes[0]->getType()) & kElem))
				return kNone;
			Record r;
			r.lanes = lanes;
			r.profit = -gatherCost(lanes);
			plan.records.push_back(r);
			return (U32)plan.records.size() - 1;
		}

		// a Load record when loadShape and loadState accept the lanes
		U32 Planner::loads(const Lanes& lanes) {
			Record r;
			r.lanes = lanes;
			r.kind = Kind::Load;
			if(!loadShape(lanes, r) || !loadState(lanes, r))
				return kNone;
			I32 w = (I32)lanes.size();
			r.profit = w - 2; // one load and a broadcast
			if(r.wideLoads)
				r.profit = w - (I32)r.wideLoads;
			plan.records.push_back(r);
			return (U32)plan.records.size() - 1;
		}

		B32 Planner::loadShape(const Lanes& lanes, Record& r) {
			LoadNode* l0 = dyn_cast<LoadNode>(lanes[0]);
			if(!l0)
				return false;
			U32 w = lanes.size(), lsz = l0->getType()->byteSize(s.ptrBytes);
			Addr a0 = s.addrs.get(l0->getPointer());
			B32 adjacent = true, equal = true;
			r.lowest = l0;
			r.off = a0.offset;
			for(U32 i = 0; i < w; ++i) {
				LoadNode* l = dyn_cast<LoadNode>(lanes[i]);
				if(!l || l->getType() != l0->getType() || l->getControl() != l0->getControl())
					return false;
				const Addr& a = s.addrs.get(l->getPointer());
				if(a.group != a0.group)
					return false;
				adjacent &= a.offset == a0.offset + (I64)(i * lsz);
				equal &= a.offset == a0.offset;
			}
			r.wideLoads = 0; // one load and a broadcast
			if(!equal)
				r.wideLoads = 1;
			return adjacent || equal;
		}

		B32 Planner::loadState(const Lanes& lanes, Record& r) {
			Node* m = r.lowest->getMemory();
			Node* ctrl = r.lowest->getControl();
			B32 shared = true;
			for(Node* l : lanes)
				shared &= cast<LoadNode>(l)->getMemory() == m;
			r.mem = m;
			// in a block with a guarded run, a run reads memIn, as loads of an earlier state would stay
			// live across that guard. A reduction adds no guard
			B32 open = seed.root || !s.guardedBlocks.count(ctrl);
			if(shared && open && !isInner(seed, m))
				return true;
			if(seed.root)
				return false; // a reduction has no memIn
			r.mem = seed.memIn;
			const Addr& run = s.addrs.get(seed.heads[0]->getPointer());
			for(Node* l : lanes) {
				Edge e = readsAtMemIn(cast<LoadNode>(l), run);
				if(e == Edge::Strong)
					return false;
				r.guard |= e == Edge::Weak;
			}
			return true;
		}

		Edge Planner::readsAtMemIn(LoadNode* l, const Addr& run) {
			const Addr& a = s.addrs.get(l->getPointer());
			B32 inner = isInner(seed, l->getMemory());
			// only run stores lie between, all in the run's group, so the bases decide
			if(inner && a.group != run.group) {
				if(disjointBases(a, run))
					return Edge::None;
				return Edge::Weak;
			}
			if(!inner && l->getMemory() != seed.memIn)
				return Edge::Strong; // a state above memIn
			Node *later = l->getMemory(), *earlier = seed.memIn;
			U32 size = l->getType()->byteSize(s.ptrBytes);
			for(U32 hops = 0; later != earlier; ++hops) {
				StoreNode* st = dyn_cast<StoreNode>(later);
				if(!st || hops == kMaxHops || st->getControl() != l->getControl())
					return Edge::Strong;
				const Addr& b = s.addrs.get(st->getPointer());
				if(!disjoint(a, size, b, st->getValue()->getType()->byteSize(s.ptrBytes)))
					return Edge::Strong;
				later = st->getMemory();
			}
			return Edge::None;
		}

		U32 Planner::op(const Lanes& lanes) {
			Forms f;
			if(!budget || !laneForms(lanes, f))
				return kNone;
			const OpRule* rule = ruleOf(f.first, lanes[0]);
			if(!rule)
				return kNone;
			Lanes ops[kMaxInputs];
			U32 count = operandTuples(lanes, f, ops);
			if(!count)
				return kNone;
			--budget;
			Record r;
			U32 mark = (U32)plan.records.size();
			for(U32 i = 0; i < count; ++i) {
				r.in[i] = vectorOf(ops[i]);
				if(r.in[i] == kNone) {
					plan.records.resize(mark);
					return kNone;
				}
			}
			r.lanes = lanes;
			r.kind = Kind::Op;
			r.op = f.first;
			r.profit = (I32)lanes.size() - 1;
			plan.records.push_back(r);
			return (U32)plan.records.size() - 1;
		}

		B32 Planner::laneForms(const Lanes& lanes, Forms& f) {
			for(U32 i = 0; i < lanes.size(); ++i) {
				Node* n = lanes[i];
				f.op[i] = n->getOpcode();
				if(!isArithmeticOpcode(f.op[i]))
					return false;
				for(U32 k = 0; k < n->getInputCount(); ++k)
					f.in[i][k] = n->getInput(k);
			}
			f.first = f.op[0];
			return std::count(f.op, f.op + lanes.size(), f.first) == (I64)lanes.size();
		}

		// the row of opcode op over lanes like n, null when n's input type or the target does not fit
		const OpRule* Planner::ruleOf(Opcode op, Node* n) {
			U8 types = typeBits(n->getType());
			for(const OpRule& row : kOpRules) {
				if(op < row.first || op > row.last || !(types & row.types))
					continue;
				B32 from = !row.from || (typeBits(n->getInput(0)->getType()) & row.from);
				if(!from || (row.sse41 && !s.sse41))
					return nullptr;
				return &row;
			}
			return nullptr;
		}

		// one operand tuple per vector input, or 0 when the lanes do not fit one vector op
		U32 Planner::operandTuples(const Lanes& lanes, const Forms& f, Lanes* ops) {
			U32 count = (U32)getOpcodeInfo(f.first).minInputs;
			for(U32 i = 0; i < lanes.size(); ++i) {
				if(lanes[i]->getType() != lanes[0]->getType())
					return 0;
				for(U32 k = 0; k < count; ++k) {
					if(f.in[i][k]->getType() != f.in[0][k]->getType())
						return 0;
					ops[k].push_back(f.in[i][k]);
				}
			}
			if(count == 2)
				orient(f, ops);
			return count;
		}

		// opcode, type and, for a load, its address group: what orient compares
		std::tuple<Opcode, const Type*, U32> Planner::operandKey(Node* n) {
			U32 group = 0;
			if(LoadNode* l = dyn_cast<LoadNode>(n))
				group = s.addrs.get(l->getPointer()).group;
			return {n->getOpcode(), n->getType(), group};
		}

		// swap the operands of a commutative lane when they match lane 0's in reverse order
		void Planner::orient(const Forms& f, Lanes* ops) {
			auto k0 = operandKey(ops[0][0]), k1 = operandKey(ops[1][0]);
			for(U32 i = 1; i < ops[0].size() && k0 != k1; ++i) {
				if(!getOpcodeInfo(f.op[i]).isCommutative)
					continue;
				auto kl = operandKey(ops[0][i]), kr = operandKey(ops[1][i]);
				if(kl != k0 && kr == k0 && kl == k1)
					std::swap(ops[0][i], ops[1][i]);
			}
		}

		// add the bytes of r's lanes to the runtime check of their group
		void Planner::addGuard(const Record& r) {
			const Addr& a = s.addrs.get(r.lowest->getPointer());
			I64 hi = a.offset;
			for(Node* n : r.lanes) {
				LoadNode* l = cast<LoadNode>(n);
				I64 end = s.addrs.get(l->getPointer()).offset + (I64)l->getType()->byteSize(s.ptrBytes);
				hi = std::max(hi, end);
			}
			for(GuardGroup& g : plan.guards)
				if(g.group == a.group) {
					if(a.offset < g.lo)
						g = {g.group, r.lowest->getPointer(), a.offset, g.hi};
					g.hi = std::max(g.hi, hi);
					return;
				}
			plan.guards.push_back({a.group, r.lowest->getPointer(), a.offset, hi});
		}
	} // namespace slp
} // namespace rat
