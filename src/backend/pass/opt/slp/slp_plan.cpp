#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {

		Planner::Planner(Slp& slp, const Seed& seed)
		: slp(slp),
			seed(seed) {
			plan.vectors.reserve(4 * seed.tuples.size());
		}

		B32 Planner::planSeed() {
			for(const Lanes& tuple : seed.tuples) {
				budget += kMaxOps;
				U32 index = planVector(tuple);
				if(index == kNoVector)
					return false;
				plan.tupleVectors.push_back(index);
			}
			for(const Vector& vec : plan.vectors)
				if(vec.needsGuard)
					addGuard(vec);
			return plan.guards.size() <= kMaxGuards;
		}

		U32 Planner::planVector(const Lanes& lanes) {
			for(const Vector& vec : plan.vectors)
				if(std::equal(lanes.begin(), lanes.end(), vec.lanes.begin(), vec.lanes.end()))
					return (U32)(&vec - plan.vectors.data());
			if(isSplat(lanes))
				return planGather(lanes);
			U32 index = planLoad(lanes);
			if(index == kNoVector)
				index = planOp(lanes);
			if(index == kNoVector)
				index = planGather(lanes);
			return index;
		}

		U32 Planner::planGather(const Lanes& lanes) {
			if(!(typeBits(lanes[0]->getType()) & kElem))
				return kNoVector;
			Vector vec{lanes, VectorKind::Gather};
			vec.profit = -gatherCost(lanes);
			plan.vectors.push_back(vec);
			return (U32)plan.vectors.size() - 1;
		}

		U32 Planner::planLoad(const Lanes& lanes) {
			Vector vec{lanes, VectorKind::Load};
			if(!loadShape(lanes, vec) || !loadState(lanes, vec))
				return kNoVector;
			I32 laneCount = (I32)lanes.size();
			vec.profit = laneCount - 2; // one load and a broadcast
			if(vec.wideLoads)
				vec.profit = laneCount - (I32)vec.wideLoads - vec.permuted;
			plan.vectors.push_back(vec);
			return (U32)plan.vectors.size() - 1;
		}

		B32 Planner::loadShape(const Lanes& lanes, Vector& vec) {
			LoadNode* firstLoad = dyn_cast<LoadNode>(lanes[0]);
			if(!firstLoad)
				return false;
			U32 laneCount = lanes.size();
			U32 laneSize = firstLoad->getType()->byteSize(slp.ptrBytes);
			Addr firstAddr = slp.addrs.get(firstLoad->getPointer());
			// i8 and i16 lanes, the source of a convert, load only adjacent
			B32 elem = (typeBits(firstLoad->getType()) & kElem) != 0;
			B32 adjacent = true;
			B32 equal = elem;
			vec.end = firstAddr.offset + (I64)laneSize;
			vec.lowestLoad = firstLoad;
			vec.offset = firstAddr.offset;
			for(U32 i = 0; i < laneCount; ++i) {
				LoadNode* l = dyn_cast<LoadNode>(lanes[i]);
				if(!l || l->isVolatile() || l->getType() != firstLoad->getType() ||
					 l->getControl() != firstLoad->getControl())
					return false;
				const Addr& a = slp.addrs.get(l->getPointer());
				if(a.group != firstAddr.group)
					return false;
				adjacent &= a.offset == firstAddr.offset + (I64)(i * laneSize);
				equal &= a.offset == firstAddr.offset;
				if(a.offset < vec.offset)
					vec.lowestLoad = l;
				vec.offset = std::min(vec.offset, a.offset);
				vec.end = std::max(vec.end, a.offset + (I64)laneSize);
			}
			if(!equal) // else one load and a broadcast
				vec.wideLoads = 1 + (vec.end - vec.offset > (I64)(laneCount * laneSize));
			vec.permuted = !adjacent && !equal;
			return adjacent || equal || (elem && permuteSpan(lanes, vec));
		}

		B32 Planner::permuteSpan(const Lanes& lanes, Vector& vec) {
			U32 laneCount = lanes.size();
			U32 laneSize = vec.lowestLoad->getType()->byteSize(slp.ptrBytes);
			U32 group = slp.addrs.get(vec.lowestLoad->getPointer()).group;
			Node* ctrl = vec.lowestLoad->getControl();
			I64 bytes = (I64)(vec.wideLoads * laneCount * laneSize);
			if(vec.end - vec.offset > bytes || (vec.wideLoads == 2 && laneCount * laneSize != kVecBytes))
				return false;
			for(I64 start : {vec.offset, vec.end - bytes}) {
				U32 readElements = 0; // bit per element
				vec.sel = 0;
				for(U32 i = 0; i < laneCount; ++i) {
					I64 delta = slp.addrs.get(cast<LoadNode>(lanes[i])->getPointer()).offset - start;
					if(delta % laneSize)
						return false;
					readElements |= 1u << (delta / laneSize);
					vec.sel |= (U16)(delta / laneSize << (ShuffleNode::kSourceBits * i));
				}
				B32 covered = true;
				for(U32 element = 0; element < vec.wideLoads * laneCount; ++element)
					if(!(readElements >> element & 1))
						covered &= slp.bytesRead(group, start + (I64)(element * laneSize), laneSize, ctrl);
				if(covered) {
					vec.offset = start;
					return true;
				}
			}
			return false;
		}

		B32 Planner::loadState(const Lanes& lanes, Vector& vec) {
			B32 shared = true;
			vec.mem = vec.lowestLoad->getMemory();
			for(Node* l : lanes)
				shared &= cast<LoadNode>(l)->getMemory() == vec.mem;
			if(seed.root)
				return shared; // a reduction has no memIn and adds no guard
			// in a block with a guarded run, a run reads memIn, as loads of an earlier state would stay
			// live across that guard
			if(shared && !slp.guardedBlocks.count(vec.lowestLoad->getControl()) &&
				 !isInner(seed, vec.mem))
				return true;
			vec.mem = seed.memIn;
			const Addr& runAddr = slp.addrs.get(seed.lowestStores[0]->getPointer());
			for(Node* l : lanes)
				if(!canReadMemIn(cast<LoadNode>(l), runAddr, vec.needsGuard))
					return false;
			return true;
		}

		// whether l may read memIn instead of its state, by the edge between them: none and weak
		// say yes, a weak edge also sets needsGuard, strong says no
		B32 Planner::canReadMemIn(LoadNode* l, const Addr& runAddr, B32& needsGuard) {
			const Addr& loadAddr = slp.addrs.get(l->getPointer());
			B32 inner = isInner(seed, l->getMemory());
			// only run stores lie between, all in the run's group, so the bases decide
			if(inner && loadAddr.group != runAddr.group) {
				needsGuard |= !disjointBases(loadAddr, runAddr);
				return true;
			}
			Node* later = seed.memIn;
			Node* earlier = l->getMemory();
			if(inner)
				std::swap(later, earlier);
			U32 size = l->getType()->byteSize(slp.ptrBytes);
			for(U32 hops = 0; later != earlier; ++hops) {
				StoreNode* st = dyn_cast<StoreNode>(later);
				if(!st || hops >= kMaxHops || st->isVolatile() || st->getControl() != l->getControl())
					return false;
				const Addr& storeAddr = slp.addrs.get(st->getPointer());
				if(!disjoint(loadAddr, size, storeAddr, st->getValue()->getType()->byteSize(slp.ptrBytes)))
					return false;
				later = st->getMemory();
			}
			return true;
		}

		U32 Planner::planOp(const Lanes& lanes) {
			LaneOps ops;
			if(!budget || !readLaneOps(lanes, ops) || !hasRuleForOps(ops, lanes[0]))
				return kNoVector;
			Lanes tuples[kMaxInputs];
			U32 tupleCount = operandTuples(lanes, ops, tuples);
			if(!tupleCount)
				return kNoVector;
			--budget;
			Vector vec{lanes, VectorKind::Op};
			U32 firstNew = (U32)plan.vectors.size();
			for(U32 i = 0; i < tupleCount; ++i) {
				vec.operands[i] = planVector(tuples[i]);
				if(vec.operands[i] == kNoVector) {
					plan.vectors.resize(firstNew);
					return kNoVector;
				}
			}
			vec.mainOp = ops.mainOp;
			vec.altOp = ops.altOp;
			if(isShiftOpcode(ops.mainOp))
				vec.shiftAmount = lanes[0]->getInput(1);
			// a variable shift amount costs a move to xmm
			vec.profit = (I32)lanes.size() - 1 - (vec.shiftAmount && !isa<ConstantNode>(vec.shiftAmount));
			if(ops.altOp != kNoOp)
				vec.profit = blendProfit(ops, lanes.size(), vec.sel);
			plan.vectors.push_back(vec);
			return (U32)plan.vectors.size() - 1;
		}

		B32 Planner::readLaneOps(const Lanes& lanes, LaneOps& ops) {
			Node* zero = nullptr;
			Node* ones = nullptr;
			for(U32 i = 0; i < lanes.size(); ++i) {
				Node* lane = lanes[i];
				ops.opcode[i] = lane->getOpcode();
				if(!isArithmeticOpcode(ops.opcode[i]) && ops.opcode[i] != Opcode::Select)
					return false;
				for(U32 k = 0; k < lane->getInputCount(); ++k)
					ops.input[i][k] = lane->getInput(k);
				if(ops.opcode[i] == Opcode::Neg) {
					if(!zero)
						zero = slp.fn.create<ConstantNode>(lane->getType(), 0);
					ops.opcode[i] = Opcode::Sub;
					ops.input[i][1] = ops.input[i][0];
					ops.input[i][0] = zero;
				} else if(ops.opcode[i] == Opcode::Not) {
					if(!ones)
						ones = slp.fn.create<ConstantNode>(lane->getType(), -1);
					ops.opcode[i] = Opcode::Xor;
					ops.input[i][1] = ones;
				}
			}
			ops.mainOp = ops.opcode[0];
			for(U32 i = 0; i < lanes.size(); ++i)
				if(ops.opcode[i] != ops.mainOp && ops.opcode[i] != ops.altOp) {
					if(ops.altOp != kNoOp)
						return false; // a third opcode
					ops.altOp = ops.opcode[i];
				}
			return true;
		}

		B32 Planner::hasRuleForOps(const LaneOps& ops, Node* lane0) {
			if(!hasRule(ops.mainOp, lane0))
				return false;
			if(ops.altOp == kNoOp)
				return true;
			B32 binary =
					getOpClass(ops.mainOp) == OpClass::Binary && getOpClass(ops.altOp) == OpClass::Binary;
			B32 shift = isShiftOpcode(ops.mainOp) || isShiftOpcode(ops.altOp);
			return slp.sse41 && binary && !shift && hasRule(ops.altOp, lane0);
		}

		B32 Planner::hasRule(Opcode op, Node* lane) {
			U8 laneTypes = typeBits(slp.laneType(lane));
			for(const OpRule& row : kOpRules) {
				if(op < row.first || op > row.last || !(laneTypes & row.laneTypes))
					continue;
				B32 inputOk = !row.inputTypes || (typeBits(lane->getInput(0)->getType()) & row.inputTypes);
				return inputOk && (!row.needsSse41 || slp.sse41);
			}
			return false;
		}

		// one operand tuple per vector input, or 0 when the lanes do not fit one vector op
		U32 Planner::operandTuples(const Lanes& lanes, const LaneOps& ops, Lanes* tuples) {
			U32 tupleCount = (U32)getOpcodeInfo(ops.mainOp).minInputs;
			if(isShiftOpcode(ops.mainOp))
				tupleCount = 1;
			for(U32 i = 0; i < lanes.size(); ++i) {
				if(lanes[i]->getType() != lanes[0]->getType())
					return 0;
				for(U32 input = 0; input < tupleCount; ++input) {
					if(ops.input[i][input]->getType() != ops.input[0][input]->getType())
						return 0;
					tuples[input].push_back(ops.input[i][input]);
				}
			}
			for(U32 input = 0; input < tupleCount; ++input) {
				U32 bytes = lanes.size() * slp.laneType(tuples[input][0])->byteSize(slp.ptrBytes);
				if(bytes > kVecBytes || bytes < 4) // at most a vector, at least a movd
					return 0;
			}
			if(isShiftOpcode(ops.mainOp) && !isUniformShift(lanes))
				return 0;
			if(ops.mainOp == Opcode::Select && slp.laneType(tuples[0][0]) != lanes[0]->getType())
				return 0;
			if(tupleCount == 2)
				orientOperands(ops, tuples);
			return tupleCount;
		}

		B32 Planner::isUniformShift(const Lanes& lanes) {
			Node* amount = lanes[0]->getInput(1);
			for(Node* n : lanes)
				if(n->getInput(1) != amount)
					return false;
			const ConstantNode* constant = dyn_cast<ConstantNode>(amount);
			return !constant || (constant->getValue() >= 0 &&
													 constant->getValue() < (I64)lanes[0]->getType()->getIntWidth());
		}

		// opcode, type and, for a load, its address group: what orientOperands compares
		std::tuple<Opcode, const Type*, U32> Planner::operandKey(Node* n) {
			U32 group = 0;
			if(LoadNode* l = dyn_cast<LoadNode>(n))
				group = slp.addrs.get(l->getPointer()).group;
			return {n->getOpcode(), n->getType(), group};
		}

		// swap the operands of a commutative lane when they match lane 0's in reverse order
		void Planner::orientOperands(const LaneOps& ops, Lanes* tuples) {
			auto key0 = operandKey(tuples[0][0]);
			auto key1 = operandKey(tuples[1][0]);
			for(U32 i = 1; i < tuples[0].size() && key0 != key1; ++i) {
				if(!getOpcodeInfo(ops.opcode[i]).isCommutative)
					continue;
				auto left = operandKey(tuples[0][i]);
				auto right = operandKey(tuples[1][i]);
				if(left != key0 && right == key0 && left == key1)
					std::swap(tuples[0][i], tuples[1][i]);
			}
		}

		I32 Planner::blendProfit(const LaneOps& ops, U32 laneCount, U16& sel) {
			constexpr Opcode kAddSub[2] = {Opcode::FSub, Opcode::FAdd}; // lane i % 2 of an addsubps
			B32 addSub = true;
			sel = 0;
			for(U32 i = 0; i < laneCount; ++i) {
				B32 fromAlt = ops.opcode[i] == ops.altOp;
				sel |= (U16)((i + laneCount * fromAlt) << (ShuffleNode::kSourceBits * i));
				addSub &= ops.opcode[i] == kAddSub[i % 2];
			}
			return (I32)laneCount - 3 + 2 * (I32)addSub;
		}

		// add the bytes of vec's lanes to the runtime check of their group
		void Planner::addGuard(const Vector& vec) {
			const Addr& lowest = slp.addrs.get(vec.lowestLoad->getPointer());
			for(GuardRange& range : plan.guards)
				if(range.group == lowest.group) {
					if(lowest.offset < range.begin)
						range = {range.group, vec.lowestLoad->getPointer(), lowest.offset, range.end};
					range.end = std::max(range.end, vec.end);
					return;
				}
			plan.guards.push_back({lowest.group, vec.lowestLoad->getPointer(), lowest.offset, vec.end});
		}

		// some load at ctrl reads each 4-byte piece of [offset, offset + size) of group
		B32 Slp::bytesRead(U32 group, I64 offset, U32 size, const Node* ctrl) {
			if(!readsIndexed)
				indexReads();
			auto it = reads.find(pairKey(ctrl->getId(), group));
			for(U32 piece = 0; piece < size; piece += kReadPiece)
				if(it == reads.end() || !it->second.count(offset + (I64)piece))
					return false;
			return true;
		}

		void Slp::indexReads() {
			readsIndexed = true;
			for(Node* n : fn) {
				LoadNode* load = dyn_cast<LoadNode>(n);
				U32 size = 0;
				if(load && !load->isVolatile())
					size = load->getType()->byteSize(ptrBytes);
				for(U32 piece = 0; size % kReadPiece == 0 && piece < size; piece += kReadPiece) {
					const Addr& addr = addrs.get(load->getPointer());
					reads[pairKey(load->getControl()->getId(), addr.group)].insert(addr.offset + (I64)piece);
				}
			}
		}
	} // namespace slp
} // namespace rat
