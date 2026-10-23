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
				if(std::equal(lanes.begin(), lanes.end(), r.lanes.begin(), r.lanes.end()))
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
				r.profit = w - (I32)r.wideLoads - r.shuffle;
			plan.records.push_back(r);
			return (U32)plan.records.size() - 1;
		}

		B32 Planner::loadShape(const Lanes& lanes, Record& r) {
			LoadNode* l0 = dyn_cast<LoadNode>(lanes[0]);
			if(!l0)
				return false;
			U32 w = lanes.size(), lsz = l0->getType()->byteSize(s.ptrBytes);
			Addr a0 = s.addrs.get(l0->getPointer());
			// i8 and i16 lanes, the source of a convert, load only adjacent
			B32 elem = (typeBits(l0->getType()) & kElem) != 0, adjacent = true, equal = elem;
			r.hi = a0.offset + (I64)lsz;
			r.lowest = l0;
			r.off = a0.offset;
			for(U32 i = 0; i < w; ++i) {
				LoadNode* l = dyn_cast<LoadNode>(lanes[i]);
				if(!l || l->isVolatile() || l->getType() != l0->getType() ||
					 l->getControl() != l0->getControl())
					return false;
				const Addr& a = s.addrs.get(l->getPointer());
				if(a.group != a0.group)
					return false;
				adjacent &= a.offset == a0.offset + (I64)(i * lsz);
				equal &= a.offset == a0.offset;
				if(a.offset < r.off)
					r.lowest = l;
				r.off = std::min(r.off, a.offset);
				r.hi = std::max(r.hi, a.offset + (I64)lsz);
			}
			if(!equal) // else one load and a broadcast
				r.wideLoads = 1 + (r.hi - r.off > (I64)(w * lsz));
			r.shuffle = !adjacent && !equal;
			return adjacent || equal || (elem && permuteSpan(lanes, r));
		}

		B32 Planner::permuteSpan(const Lanes& lanes, Record& r) {
			U32 w = lanes.size(), lsz = r.lowest->getType()->byteSize(s.ptrBytes);
			U32 group = s.addrs.get(r.lowest->getPointer()).group;
			I64 bytes = (I64)(r.wideLoads * w * lsz);
			if(r.hi - r.off > bytes || (r.wideLoads == 2 && w * lsz != kVecBytes))
				return false;
			for(I64 b : {r.off, r.hi - bytes}) {
				U32 read = 0; // bit per element
				r.sel = 0;
				for(U32 i = 0; i < w; ++i) {
					I64 d = s.addrs.get(cast<LoadNode>(lanes[i])->getPointer()).offset - b;
					if(d % lsz)
						return false;
					read |= 1u << (d / lsz);
					r.sel |= (U16)(d / lsz << (ShuffleNode::kSourceBits * i));
				}
				B32 covered = true;
				for(U32 j = 0; j < r.wideLoads * w; ++j)
					if(!(read >> j & 1))
						covered &= s.bytesRead(group, b + (I64)(j * lsz), lsz, r.lowest->getControl());
				if(covered) {
					r.off = b;
					return true;
				}
			}
			return false;
		}

		B32 Planner::loadState(const Lanes& lanes, Record& r) {
			B32 shared = true;
			r.mem = r.lowest->getMemory();
			for(Node* l : lanes)
				shared &= cast<LoadNode>(l)->getMemory() == r.mem;
			if(seed.root)
				return shared; // a reduction has no memIn and adds no guard
			// in a block with a guarded run, a run reads memIn, as loads of an earlier state would stay
			// live across that guard
			if(shared && !s.guardedBlocks.count(r.lowest->getControl()) && !isInner(seed, r.mem))
				return true;
			r.mem = seed.memIn;
			const Addr& run = s.addrs.get(seed.heads[0]->getPointer());
			for(Node* l : lanes)
				if(!readsAtMemIn(cast<LoadNode>(l), run, r))
					return false;
			return true;
		}

		// whether l may read memIn instead of its state, by the edge between them: none and weak
		// say yes, a weak edge also guards r, strong says no
		B32 Planner::readsAtMemIn(LoadNode* l, const Addr& run, Record& r) {
			const Addr& a = s.addrs.get(l->getPointer());
			B32 inner = isInner(seed, l->getMemory());
			// only run stores lie between, all in the run's group, so the bases decide
			if(inner && a.group != run.group) {
				r.guard |= !disjointBases(a, run);
				return true;
			}
			Node *later = seed.memIn, *earlier = l->getMemory();
			if(inner)
				std::swap(later, earlier);
			U32 size = l->getType()->byteSize(s.ptrBytes);
			for(U32 hops = 0; later != earlier; ++hops) {
				if(MemMergeNode* m = dyn_cast<MemMergeNode>(later)) {
					later = stepOverSet(m, l, hops);
					if(!later)
						return false;
					continue;
				}
				StoreNode* st = dyn_cast<StoreNode>(later);
				if(!st || hops >= kMaxHops || st->isVolatile() || st->getControl() != l->getControl())
					return false;
				const Addr& b = s.addrs.get(st->getPointer());
				if(!disjoint(a, size, b, st->getValue()->getType()->byteSize(s.ptrBytes)))
					return false;
				later = st->getMemory();
			}
			return true;
		}

		Node* Planner::stepOverSet(MemMergeNode* m, LoadNode* l, U32& hops) {
			const Addr& a = s.addrs.get(l->getPointer());
			U32 size = l->getType()->byteSize(s.ptrBytes);
			Node* before = nullptr;
			for(U32 i = 1; i < m->getInputCount(); ++i) {
				StoreNode* st = dyn_cast<StoreNode>(m->getInput(i));
				if(!st)
					continue;
				U32 stSize = st->getValue()->getType()->byteSize(s.ptrBytes);
				if(++hops > kMaxHops || st->getControl() != l->getControl() ||
					 !disjoint(a, size, s.addrs.get(st->getPointer()), stSize))
					return nullptr;
				before = st->getMemory();
			}
			return before;
		}

		U32 Planner::op(const Lanes& lanes) {
			Forms f;
			if(!budget || !laneForms(lanes, f) || !hasFormRule(f, lanes[0]))
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
			r.op2 = f.second;
			if(isShiftOpcode(f.first))
				r.amount = lanes[0]->getInput(1);
			// a variable shift amount costs a move to xmm
			r.profit = (I32)lanes.size() - 1 - (r.amount && !isa<ConstantNode>(r.amount));
			if(f.second != kNoOp)
				r.profit = blendProfit(f, lanes.size(), r.sel);
			plan.records.push_back(r);
			return (U32)plan.records.size() - 1;
		}

		B32 Planner::laneForms(const Lanes& lanes, Forms& f) {
			Node *zero = nullptr, *ones = nullptr;
			for(U32 i = 0; i < lanes.size(); ++i) {
				Node* n = lanes[i];
				f.op[i] = n->getOpcode();
				if(!isArithmeticOpcode(f.op[i]) && f.op[i] != Opcode::Select)
					return false;
				for(U32 k = 0; k < n->getInputCount(); ++k)
					f.in[i][k] = n->getInput(k);
				if(f.op[i] == Opcode::Neg) {
					if(!zero)
						zero = s.fn.create<ConstantNode>(n->getType(), 0);
					f.op[i] = Opcode::Sub;
					f.in[i][1] = f.in[i][0];
					f.in[i][0] = zero;
				} else if(f.op[i] == Opcode::Not) {
					if(!ones)
						ones = s.fn.create<ConstantNode>(n->getType(), -1);
					f.op[i] = Opcode::Xor;
					f.in[i][1] = ones;
				}
			}
			if(std::count(f.op, f.op + lanes.size(), f.op[0]) != (I64)lanes.size())
				subAsAdd(lanes, f);
			f.first = f.op[0];
			for(U32 i = 0; i < lanes.size(); ++i)
				if(f.op[i] != f.first && f.op[i] != f.second) {
					if(f.second != kNoOp)
						return false; // a third opcode
					f.second = f.op[i];
				}
			return true;
		}

		// an integer x - c as x + -c mod 2^n, so that such lanes beside adds pack as one op
		void Planner::subAsAdd(const Lanes& lanes, Forms& f) {
			for(U32 i = 0; i < lanes.size(); ++i) {
				const ConstantNode* c = dyn_cast<ConstantNode>(f.in[i][1]);
				Type* t = lanes[i]->getType();
				if(f.op[i] != Opcode::Sub || !c || !t->isInt())
					continue;
				I64 neg = (I64)(0 - (U64)c->getValue());
				f.op[i] = Opcode::Add;
				f.in[i][1] = s.fn.create<ConstantNode>(t, signExtend(neg, t->getIntWidth()));
			}
		}

		B32 Planner::hasFormRule(const Forms& f, Node* n0) {
			if(!hasRule(f.first, n0))
				return false;
			if(f.second == kNoOp)
				return true;
			B32 binary =
					getOpClass(f.first) == OpClass::Binary && getOpClass(f.second) == OpClass::Binary;
			B32 shift = isShiftOpcode(f.first) || isShiftOpcode(f.second);
			return s.sse41 && binary && !shift && hasRule(f.second, n0);
		}

		// whether a row has opcode op over lanes like n, and n's input type and the target fit it
		B32 Planner::hasRule(Opcode op, Node* n) {
			U8 types = typeBits(s.laneType(n));
			for(const OpRule& row : kOpRules) {
				if(op < row.first || op > row.last || !(types & row.types))
					continue;
				B32 from = !row.from || (typeBits(n->getInput(0)->getType()) & row.from);
				return from && (!row.sse41 || s.sse41);
			}
			return false;
		}

		// one operand tuple per vector input, or 0 when the lanes do not fit one vector op
		U32 Planner::operandTuples(const Lanes& lanes, const Forms& f, Lanes* ops) {
			U32 count = (U32)getOpcodeInfo(f.first).minInputs;
			if(isShiftOpcode(f.first))
				count = 1;
			for(U32 i = 0; i < lanes.size(); ++i) {
				if(lanes[i]->getType() != lanes[0]->getType())
					return 0;
				for(U32 k = 0; k < count; ++k) {
					if(f.in[i][k]->getType() != f.in[0][k]->getType())
						return 0;
					ops[k].push_back(f.in[i][k]);
				}
			}
			for(U32 k = 0; k < count; ++k) {
				U32 bytes = lanes.size() * s.laneType(ops[k][0])->byteSize(s.ptrBytes);
				if(bytes > kVecBytes || bytes < 4) // at most a vector, at least a movd
					return 0;
			}
			if(isShiftOpcode(f.first) && !uniformShift(lanes))
				return 0;
			if(f.first == Opcode::Select && s.laneType(ops[0][0]) != lanes[0]->getType())
				return 0;
			if(count == 2)
				orient(f, ops);
			return count;
		}

		// every lane shifts by one node, below the lane width when it is a constant
		B32 Planner::uniformShift(const Lanes& lanes) {
			Node* amount = lanes[0]->getInput(1);
			for(Node* n : lanes)
				if(n->getInput(1) != amount)
					return false;
			const ConstantNode* c = dyn_cast<ConstantNode>(amount);
			return !c || (c->getValue() >= 0 && c->getValue() < (I64)lanes[0]->getType()->getIntWidth());
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

		I32 Planner::blendProfit(const Forms& f, U32 w, U16& sel) {
			B32 addSub = true;
			sel = 0;
			for(U32 i = 0; i < w; ++i) {
				sel |= (U16)((i + w * (f.op[i] == f.second)) << (ShuffleNode::kSourceBits * i));
				Opcode addSubOp = Opcode::FSub;
				if(i % 2)
					addSubOp = Opcode::FAdd;
				addSub &= f.op[i] == addSubOp;
			}
			return (I32)w - 3 + 2 * (I32)addSub;
		}

		// add the bytes of r's lanes to the runtime check of their group
		void Planner::addGuard(const Record& r) {
			const Addr& a = s.addrs.get(r.lowest->getPointer());
			for(GuardGroup& g : plan.guards)
				if(g.group == a.group) {
					if(a.offset < g.lo)
						g = {g.group, r.lowest->getPointer(), a.offset, g.hi};
					g.hi = std::max(g.hi, r.hi);
					return;
				}
			plan.guards.push_back({a.group, r.lowest->getPointer(), a.offset, r.hi});
		}

		// some load at ctrl reads each 4-byte piece of [off, off + size) of group
		B32 Slp::bytesRead(U32 group, I64 off, U32 size, const Node* ctrl) {
			if(!readsIndexed)
				indexReads();
			auto it = reads.find(pairKey(ctrl->getId(), group));
			for(U32 b = 0; b < size; b += kReadPiece)
				if(it == reads.end() || !it->second.count(off + (I64)b))
					return false;
			return true;
		}

		void Slp::indexReads() {
			readsIndexed = true;
			for(Node* n : fn) {
				LoadNode* l = dyn_cast<LoadNode>(n);
				U32 size = 0;
				if(l && !l->isVolatile())
					size = l->getType()->byteSize(ptrBytes);
				for(U32 b = 0; size % kReadPiece == 0 && b < size; b += kReadPiece) {
					const Addr& a = addrs.get(l->getPointer());
					reads[pairKey(l->getControl()->getId(), a.group)].insert(a.offset + (I64)b);
				}
			}
		}
	} // namespace slp
} // namespace rat
