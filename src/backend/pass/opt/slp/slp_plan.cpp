#include "pass/opt/slp/slp_pack_detail.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace slp {
		namespace detail {
			// lanes of one opcode in [first, last] become one vector op of that opcode
			// a shift takes one amount for all lanes, a select takes a mask, every other input is a tuple
			struct OpRule {
				Opcode first;
				Opcode last;
				U8 laneTypes;	 // a compare lane is its mask
				U8 inputTypes; // of input 0 of a compare or convert, 0 for no check
				B32 needsSse41;
			};

			// clang-format off
			constexpr OpRule kOpRules[] = {
				{Opcode::Add, Opcode::Sub, kI32I64, 0, false},
				{Opcode::Mul, Opcode::Mul, kI32, 0, true},							// pmulld, no i64 mul
				{Opcode::And, Opcode::Xor, kI32I64, 0, false},
				{Opcode::Shl, Opcode::LShr, kI32I64, 0, false},
				{Opcode::AShr, Opcode::AShr, kI32, 0, false},						// psrad, no i64 sar
				{Opcode::FAdd, Opcode::FDiv, kFp, 0, false},
				{Opcode::FNeg, Opcode::FNeg, kFp, 0, false},
				{Opcode::Eq, Opcode::Ne, kI32I64, kI32I64, false},			// pcmpeqd, pcmpeqq
				{Opcode::Slt, Opcode::Ule, kI32, kI32, false},					// pcmpgtd, pminud
				{Opcode::FEq, Opcode::FGe, kI32I64, kFp, false},				// cmpps, cmppd
				{Opcode::SExt, Opcode::ZExt, kI32I64, kI8 | kI16 | kI32, true}, // pmovsx/zx
				{Opcode::SIToFP, Opcode::SIToFP, kFp, kI32, false},			// cvtdq2ps/pd
				{Opcode::FPToSI, Opcode::FPToSI, kI32, kFp, false},			// cvttps2dq/pd2dq
				{Opcode::FPExt, Opcode::FPExt, kF64, kF32, false},			// cvtps2pd
				{Opcode::FPTrunc, Opcode::FPTrunc, kF32, kF64, false},	// cvtpd2ps
				{Opcode::Select, Opcode::Select, kI32I64, 0, true},			// blendv
			};
			static_assert((I32)Opcode::Sub - (I32)Opcode::Add == 1 &&
										(I32)Opcode::Xor - (I32)Opcode::And == 2 &&
										(I32)Opcode::LShr - (I32)Opcode::Shl == 1 &&
										(I32)Opcode::FDiv - (I32)Opcode::FAdd == 3 &&
										(I32)Opcode::Ne - (I32)Opcode::Eq == 1 &&
										(I32)Opcode::Ule - (I32)Opcode::Slt == 3 &&
										(I32)Opcode::FGe - (I32)Opcode::FEq == 5 &&
										(I32)Opcode::ZExt - (I32)Opcode::SExt == 1,
										"kOpRules ranges follow ir/opcode.h");
			// clang-format on
		} // namespace detail

		Plan::Plan(Context& cx, LoadFacts& facts, const GuardedRuns& guardedRuns, const Seed& seed)
		: seed(seed),
			cx(cx),
			facts(facts),
			guardedRuns(guardedRuns) {
			vectors.reserve(4 * seed.tuples.size());
		}

		B32 Plan::build() {
			for(const Lanes& tuple : seed.tuples) {
				budget += kMaxOps;
				U32 index = planVector(tuple);
				if(index == kNoVector)
					return false;
				tupleVectors.push_back(index);
			}
			for(const Vector& vec : vectors)
				if(vec.needsGuard)
					addGuard(vec);
			if(guards.empty())
				return true;
			// a guarded run splits its block, and the loads above its guard stay live across the splits
			return guards.size() <= kMaxGuards && !guardedRuns.isFull(seed.stores.back()->getControl());
		}

		U32 Plan::planVector(const Lanes& lanes) {
			for(const Vector& vec : vectors)
				if(std::equal(lanes.begin(), lanes.end(), vec.lanes.begin(), vec.lanes.end()))
					return (U32)(&vec - vectors.data());
			if(isSplat(lanes))
				return planGather(lanes);
			U32 index = planLoad(lanes);
			if(index == kNoVector)
				index = planOp(lanes);
			if(index == kNoVector)
				index = planGather(lanes);
			return index;
		}

		U32 Plan::planGather(const Lanes& lanes) {
			if(!(typeBits(lanes[0]->getType()) & kElem))
				return kNoVector;
			Vector vec{lanes, VectorKind::Gather};
			vec.profit = -gatherCost(lanes, seed.root != nullptr);
			vectors.push_back(vec);
			return (U32)vectors.size() - 1;
		}

		B32 isSplat(const Lanes& lanes) {
			for(Node* n : lanes)
				if(n != lanes[0])
					return false;
			return !isa<ConstantNode>(lanes[0]);
		}

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

		U32 Plan::planOp(const Lanes& lanes) {
			LaneOps ops;
			if(!budget || !readLaneOps(lanes, ops) || !hasRuleForOps(ops, lanes[0]))
				return kNoVector;
			Lanes tuples[kMaxInputs];
			U32 tupleCount = operandTuples(lanes, ops, tuples);
			if(!tupleCount)
				return kNoVector;
			--budget;
			Vector vec{lanes, VectorKind::Op};
			U32 firstNew = (U32)vectors.size();
			for(U32 i = 0; i < tupleCount; ++i) {
				vec.operands[i] = planVector(tuples[i]);
				if(vec.operands[i] == kNoVector) {
					vectors.resize(firstNew);
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
			vectors.push_back(vec);
			return (U32)vectors.size() - 1;
		}

		B32 Plan::readLaneOps(const Lanes& lanes, LaneOps& ops) {
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
					// made even when planOp rejects the lanes, dropping it shifts later node ids
					if(!zero)
						zero = cx.fn.create<ConstantNode>(lane->getType(), 0);
					ops.opcode[i] = Opcode::Sub;
					ops.input[i][1] = ops.input[i][0];
					ops.input[i][0] = zero;
				} else if(ops.opcode[i] == Opcode::Not) {
					if(!ones)
						ones = cx.fn.create<ConstantNode>(lane->getType(), -1);
					ops.opcode[i] = Opcode::Xor;
					ops.input[i][1] = ones;
				}
			}
			return pickOpcodes(ops, lanes.size());
		}

		// mainOp is the opcode of lane 0, altOp the one other opcode, false for a third
		B32 pickOpcodes(LaneOps& ops, U32 laneCount) {
			ops.mainOp = ops.opcode[0];
			for(U32 i = 0; i < laneCount; ++i)
				if(ops.opcode[i] != ops.mainOp && ops.opcode[i] != ops.altOp) {
					if(ops.altOp != kNoOp)
						return false; // a third opcode
					ops.altOp = ops.opcode[i];
				}
			return true;
		}

		B32 Plan::hasRuleForOps(const LaneOps& ops, Node* lane0) {
			if(!hasRule(ops.mainOp, lane0))
				return false;
			if(ops.altOp == kNoOp)
				return true;
			B32 binary =
					getOpClass(ops.mainOp) == OpClass::Binary && getOpClass(ops.altOp) == OpClass::Binary;
			B32 shift = isShiftOpcode(ops.mainOp) || isShiftOpcode(ops.altOp);
			return cx.sse41 && binary && !shift && hasRule(ops.altOp, lane0);
		}

		B32 Plan::hasRule(Opcode op, Node* lane) {
			U8 laneTypes = typeBits(cx.laneType(lane));
			for(const detail::OpRule& row : detail::kOpRules) {
				if(op < row.first || op > row.last || !(laneTypes & row.laneTypes))
					continue;
				B32 inputOk = !row.inputTypes || (typeBits(lane->getInput(0)->getType()) & row.inputTypes);
				return inputOk && (!row.needsSse41 || cx.sse41);
			}
			return false;
		}

		// one operand tuple per vector input, or 0 when the lanes do not fit one vector op
		U32 Plan::operandTuples(const Lanes& lanes, const LaneOps& ops, Lanes* tuples) {
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
				U32 bytes = lanes.size() * cx.laneType(tuples[input][0])->byteSize(cx.ptrBytes);
				if(bytes > kVecBytes || bytes < 4) // at most a vector, at least a movd
					return 0;
			}
			if(isShiftOpcode(ops.mainOp) && !isUniformShift(lanes))
				return 0;
			if(ops.mainOp == Opcode::Select && cx.laneType(tuples[0][0]) != lanes[0]->getType())
				return 0;
			if(tupleCount == 2)
				orientOperands(ops, tuples);
			return tupleCount;
		}

		B32 isUniformShift(const Lanes& lanes) {
			Node* amount = lanes[0]->getInput(1);
			for(Node* n : lanes)
				if(n->getInput(1) != amount)
					return false;
			const ConstantNode* constant = dyn_cast<ConstantNode>(amount);
			return !constant || (constant->getValue() >= 0 &&
													 constant->getValue() < (I64)lanes[0]->getType()->getIntWidth());
		}

		// opcode, type and, for a load, its address group: what orientOperands compares
		std::tuple<Opcode, const Type*, U32> Plan::operandKey(Node* n) {
			U32 group = 0;
			if(LoadNode* l = dyn_cast<LoadNode>(n))
				group = cx.addrs.get(l->getPointer()).group;
			return {n->getOpcode(), n->getType(), group};
		}

		// swap the operands of a commutative lane when they match lane 0's in reverse order
		void Plan::orientOperands(const LaneOps& ops, Lanes* tuples) {
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

		I32 blendProfit(const LaneOps& ops, U32 laneCount, U16& sel) {
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
	} // namespace slp
} // namespace rat
