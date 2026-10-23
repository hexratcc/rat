#include "pass/opt/slp/slp_pack.h"

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

		Plan::Plan(Slp& slp, const Seed& seed)
		: slp(slp),
			seed(seed) {
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
			auto it = slp.guardedRuns.find(seed.stores.back()->getControl());
			B32 blockFull = it != slp.guardedRuns.end() && it->second >= kMaxGuardedRuns;
			return guards.size() <= kMaxGuards && !blockFull;
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

		U32 Plan::planLoad(const Lanes& lanes) {
			Vector vec{lanes, VectorKind::Load};
			if(!loadShape(lanes, vec) || !loadState(lanes, vec) || forwardBlocked(vec))
				return kNoVector;
			I32 laneCount = (I32)lanes.size();
			vec.profit = laneCount - 2; // one load and a broadcast
			if(vec.wideLoads)
				vec.profit = laneCount - (I32)vec.wideLoads - vec.permuted;
			vectors.push_back(vec);
			return (U32)vectors.size() - 1;
		}

		B32 Plan::loadShape(const Lanes& lanes, Vector& vec) {
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

		B32 Plan::permuteSpan(const Lanes& lanes, Vector& vec) {
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

		B32 Plan::loadState(const Lanes& lanes, Vector& vec) {
			B32 shared = true;
			vec.mem = vec.lowestLoad->getMemory();
			for(Node* l : lanes)
				shared &= cast<LoadNode>(l)->getMemory() == vec.mem;
			if(seed.root)
				return shared; // a reduction has no memIn and adds no guard
			// in a block with a guarded run, a run reads memIn, as loads of an earlier state would stay
			// live across that guard
			if(shared && !slp.guardedRuns.count(vec.lowestLoad->getControl()) && !isInner(seed, vec.mem))
				return true;
			vec.mem = memIn(seed);
			const Addr& runAddr = slp.addrs.get(seed.lowestStores[0]->getPointer());
			for(Node* l : lanes)
				if(!canReadMemIn(cast<LoadNode>(l), runAddr, vec.needsGuard))
					return false;
			return true;
		}

		// whether l may read memIn instead of its state, by the edge between them: none and weak
		// say yes, a weak edge also sets needsGuard, strong says no
		B32 Plan::canReadMemIn(LoadNode* l, const Addr& runAddr, B32& needsGuard) {
			const Addr& loadAddr = slp.addrs.get(l->getPointer());
			B32 inner = isInner(seed, l->getMemory());
			// only run stores lie between, all in the run's group, so the bases decide
			if(inner && loadAddr.group != runAddr.group) {
				needsGuard |= !disjointBases(loadAddr, runAddr);
				return true;
			}
			Node* later = memIn(seed);
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

		// a wide load forwards only from one older store that holds all its bytes, else it waits about
		// 11 cycles on Zen 3 until the stores commit, so each youngest writer must hold all its bytes
		B32 Plan::forwardBlocked(const Vector& vec) {
			U32 bytes = (U32)vec.lanes.size() * vec.lowestLoad->getType()->byteSize(slp.ptrBytes);
			Access load{slp.addrs.get(vec.lowestLoad->getPointer()), bytes};
			for(U32 k = 0; k < vec.wideLoads; ++k) {
				load.addr.offset = vec.offset + (I64)(k * bytes);
				SmallList<Node*, kMaxStlfHops> walk = {vec.mem};
				for(U32 i = 0; i < walk.size() && i < kMaxStlfHops; ++i)
					if(splitsLoad(walk[i], load, walk))
						return true;
			}
			return false;
		}

		// whether state writes some but not all bytes of load, or is a call that writes them, and else
		// add to walk the states before it, which may hold the youngest writer
		B32 Plan::splitsLoad(Node* state, const Access& load, SmallList<Node*, kMaxStlfHops>& walk) {
			if(PhiNode* phi = dyn_cast<PhiNode>(state)) {
				// a backedge brings stores of the previous iteration, whose addresses the walk cannot
				// compare, so a loop header gives only its entry, input 0
				U32 count = phi->getValueCount();
				if(phi->getRegion()->isLoopHeader())
					count = 1;
				for(U32 i = 0; i < count; ++i)
					walk.push_back(phi->getValue(i));
				return false;
			}
			ProjNode* proj = dyn_cast<ProjNode>(state);
			if(proj && isa<CallNode>(proj->getProducer()))
				return callWrites(cast<CallNode>(proj->getProducer()), load.addr);
			StoreNode* st = dyn_cast<StoreNode>(state);
			if(!st)
				return false; // the entry state or another writer, taken as old
			Access store = accessOf(slp.addrs, slp.ptrBytes, st);
			if(disjoint(load.addr, load.size, store.addr, store.size)) {
				walk.push_back(st->getMemory());
				return false;
			}
			// a store of another group that may overlap ends the walk
			I64 loadEnd = load.addr.offset + (I64)load.size;
			I64 storeEnd = store.addr.offset + (I64)store.size;
			B32 holdsAll = store.addr.offset <= load.addr.offset && storeEnd >= loadEnd;
			return store.addr.group == load.addr.group && !holdsAll;
		}

		// an object that no store of the function writes, an out parameter or a struct return, gets
		// its bytes from the calls that take its address, with stores the walk cannot see, and a
		// callee writes a struct field by field
		B32 Plan::callWrites(const CallNode* call, const Addr& addr) {
			if(!addr.object || slp.storesTo(addr.object))
				return false;
			for(U32 i = 0; i < call->getArgCount(); ++i) {
				Node* arg = call->getArg(i);
				if(arg->getType()->isPtr() && slp.addrs.get(arg).object == addr.object)
					return true;
			}
			return false;
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

		B32 Plan::hasRuleForOps(const LaneOps& ops, Node* lane0) {
			if(!hasRule(ops.mainOp, lane0))
				return false;
			if(ops.altOp == kNoOp)
				return true;
			B32 binary =
					getOpClass(ops.mainOp) == OpClass::Binary && getOpClass(ops.altOp) == OpClass::Binary;
			B32 shift = isShiftOpcode(ops.mainOp) || isShiftOpcode(ops.altOp);
			return slp.sse41 && binary && !shift && hasRule(ops.altOp, lane0);
		}

		B32 Plan::hasRule(Opcode op, Node* lane) {
			U8 laneTypes = typeBits(slp.laneType(lane));
			for(const detail::OpRule& row : detail::kOpRules) {
				if(op < row.first || op > row.last || !(laneTypes & row.laneTypes))
					continue;
				B32 inputOk = !row.inputTypes || (typeBits(lane->getInput(0)->getType()) & row.inputTypes);
				return inputOk && (!row.needsSse41 || slp.sse41);
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
				group = slp.addrs.get(l->getPointer()).group;
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

		// add the bytes of vec's lanes to the runtime check of their group
		void Plan::addGuard(const Vector& vec) {
			const Addr& lowest = slp.addrs.get(vec.lowestLoad->getPointer());
			for(GuardRange& range : guards)
				if(range.group == lowest.group) {
					if(lowest.offset < range.begin)
						range = {range.group, vec.lowestLoad->getPointer(), lowest.offset, range.end};
					range.end = std::max(range.end, vec.end);
					return;
				}
			guards.push_back({lowest.group, vec.lowestLoad->getPointer(), lowest.offset, vec.end});
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

		B32 Slp::storesTo(U32 object) {
			if(!storesIndexed)
				indexStores();
			return storedObjects.count(object) != 0;
		}

		void Slp::indexStores() {
			storesIndexed = true;
			for(Node* n : fn)
				if(StoreNode* st = dyn_cast<StoreNode>(n))
					storedObjects.insert(addrs.get(st->getPointer()).object);
		}
	} // namespace slp
} // namespace rat
