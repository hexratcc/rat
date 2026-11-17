#include "pass/opt/slp.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"
#include "pass/opt/load_canon.h"
#include "target/target.h"

#include <ostream>

namespace rat {
	namespace detail {
		// type bits of a scalar
		enum TypeBit : U8 {
			kInt8 = 1,
			kInt16 = 2,
			kInt32 = 4,
			kInt64 = 8,
			kFloat32 = 16,
			kFloat64 = 32,
			kPtr = 64,
			kInt32Int64 = kInt32 | kInt64,
			kIntUpTo32 = kInt8 | kInt16 | kInt32,
			kFloat = kFloat32 | kFloat64,
			kElem = kInt32Int64 | kFloat | kPtr, // what a stored or gathered lane can be
		};

		constexpr U32 kValueInput = 3; // of a store

		struct LaneRule {
			Opcode first;
			Opcode last;
			U8 laneTypes;	 // of a lane, of its operands for a compare
			U8 inputTypes; // of input 0 of a convert, 0 for no check
		};

		// clang-format off
		constexpr LaneRule kLaneRules[] = {
			{Opcode::Add,     Opcode::Sub,     kInt32Int64, 0},
			{Opcode::Mul,     Opcode::Mul,     kInt32,      0},          // pmulld, i32 only
			{Opcode::And,     Opcode::LShr,    kInt32Int64, 0},          // logic, shl, lshr
			{Opcode::AShr,    Opcode::AShr,    kInt32,      0},          // psrad, no i64 sar
			{Opcode::FAdd,    Opcode::FDiv,    kFloat,      0},
			{Opcode::FNeg,    Opcode::FNeg,    kFloat,      0},
			{Opcode::Eq,      Opcode::Ne,      kInt32Int64, 0},          // pcmpeqd/q
			{Opcode::Slt,     Opcode::Ule,     kInt32,      0},          // pcmpgtd, pminud
			{Opcode::FEq,     Opcode::FGe,     kFloat,      0},          // cmpps/pd
			{Opcode::SExt,    Opcode::ZExt,    kInt32Int64, kIntUpTo32}, // pmovsx/zx
			{Opcode::SIToFP,  Opcode::SIToFP,  kFloat,      kInt32},     // cvtdq2ps/pd
			{Opcode::FPToSI,  Opcode::FPToSI,  kInt32,      kFloat},     // cvttps2dq/pd2dq
			{Opcode::FPExt,   Opcode::FPExt,   kFloat64,    kFloat32},   // cvtps2pd
			{Opcode::FPTrunc, Opcode::FPTrunc, kFloat32,    kFloat64},   // cvtpd2ps
			{Opcode::Select,  Opcode::Select,  kInt32Int64, 0},          // blendv
		};
		// clang-format on
	} // namespace detail

	U8 SlpPass::typeBits(const Type* t) {
		using namespace detail;
		if(t->isPtr())
			return kPtr;
		if(t->isFloat()) {
			U32 width = t->getFloatWidth();
			return (width == 32) * kFloat32 | (width == 64) * kFloat64;
		}
		U8 narrow = t->isInt(8) * kInt8 | t->isInt(16) * kInt16;
		return narrow | t->isInt(32) * kInt32 | t->isInt(64) * kInt64;
	}

	const C8* SlpPass::name() const { return "slp"; }

	void SlpPass::printStats(const Module&, std::ostream& os) const {
		os << "slp: windows " << packedWindows;
		os << " reductions " << packedReductions << "\n";
	}

	U32 SlpPass::runOnFunction(Function& function, const TargetInfo& target) {
		fn = &function;
		ptrBytes = target.getPointerSizeInBytes();
		LoadCanonPass::Handoff handoff = LoadCanonPass::takeHandoff(function);
		addrs = std::move(handoff.addrs);
		storedObjects.clear();
		for(const List<StoreNode*>& chain : handoff.chains)
			for(StoreNode* st : chain)
				storedObjects.insert(addrs.get(st->getPointer()).object);
		U32 before = packedWindows + packedReductions;
		List<B32> packed; // per chain
		for(const List<StoreNode*>& chain : handoff.chains)
			packed.push_back(packChain(chain));
		// a chain that packs nothing gets its loads back, else they could load long before their use
		LoadCanonPass::unhoistLoads(handoff.hoists, packed);
		function.eliminateDeadNodes();
		// a copy of the nodes, as a pack adds nodes
		for(Node* n : List<Node*>(function.begin(), function.end()))
			packReduction(n);
		// the replaced scalars and the vectors of rejected packs
		function.eliminateDeadNodes();
		return packedWindows + packedReductions - before;
	}

	// cut the chain into stretches
	B32 SlpPass::packChain(const List<StoreNode*>& chain) {
		U32 windowsBefore = packedWindows;
		List<StoreAccess> stretch;
		for(StoreNode* st : chain) {
			U32 size = st->getValue()->getType()->byteSize(ptrBytes);
			Addr addr = addrs.get(st->getPointer());
			StoreAccess access{st, addr, size};
			StoreNode* prev = stretch.empty() ? nullptr : stretch.back().store;
			B32 prevUnread = !prev || prev->getUsers().size() == 1;
			B32 joins = prevUnread && stretch.size() < kMaxStretch;
			for(const StoreAccess& member : stretch)
				joins = joins && disjoint(addr, size, member.addr, member.size);
			if(!joins)
				packStretch(stretch);
			stretch.push_back(access); // AddrCache gives every store a group
		}
		packStretch(stretch);
		return packedWindows != windowsBefore;
	}

	// pack the windows of the stretch in address order, then empty it
	void SlpPass::packStretch(List<StoreAccess>& stretch) {
		vectors.clear(); // the packs of a stretch share vectors
		vectorsOf = Map<const Node*, List<U32>>();
		std::sort(stretch.begin(), stretch.end(), byAddress);
		for(U32 first = 0; first < stretch.size();) {
			const StoreAccess& low = stretch[first];
			Type* type = low.store->getValue()->getType();
			B32 elem = typeBits(type) & detail::kElem;
			U32 maxCount = elem ? kVecBytes / low.size : 1;
			maxCount = std::min(maxCount, (U32)stretch.size() - first);
			U32 count = 1;
			for(; count < maxCount; ++count) {
				const Addr& addr = stretch[first + count].addr;
				I64 nextOffset = low.addr.offset + count * low.size;
				if(addr.group != low.addr.group || addr.offset != nextOffset)
					break;
			}
			// an odd count rounds down to even, so a lone store tries nothing
			U32 packedCount = packWindow(stretch.data() + first, count & ~1u);
			first += std::max(packedCount, 1u);
		}
		stretch.clear();
	}

	B32 SlpPass::byAddress(const StoreAccess& a, const StoreAccess& b) {
		if(a.addr.group != b.addr.group)
			return a.addr.group < b.addr.group;
		return a.addr.offset < b.addr.offset;
	}

	U32 SlpPass::packWindow(const StoreAccess* window, U32 count) {
		if(!count)
			return 0;
		Lanes values;
		Set<const Node*> stores;
		for(U32 k = 0; k < count; ++k) {
			values.push_back(window[k].store->getValue());
			stores.insert(window[k].store);
		}
		List<Node*> roots = tryPack({values}, std::move(stores), (I32)count - 1);
		if(roots.empty())
			return packWindow(window, count - 2);
		window[0].store->setInput(detail::kValueInput, roots[0]);
		for(U32 k = 1; k < count; ++k) {
			StoreNode* st = window[k].store;
			st->replaceAllUsesWith(st->getMemory());
			st->clearInputs(); // a store with no control is dead
		}
		++packedWindows;
		return count;
	}

	void SlpPass::packReduction(Node* root) {
		Opcode op = root->getOpcode();
		Type* type = root->getType();
		if(!root->hasUsers())
			return; // a dead node is no root
		if(!root->isCommutative())
			return;
		if(!(typeBits(type) & detail::kInt32Int64) || !hasRule(root))
			return;
		for(Node* u : root->getUsers())
			if(u->getOpcode() == op && u->getType() == type)
				return;
		Set<const Node*> steps;
		List<Node*> terms = flatten(root, steps);
		U32 laneCount = kVecBytes / type->byteSize(ptrBytes);
		U32 vectorCount = (U32)terms.size() / laneCount;
		// most roots have fewer terms than lanes, they return before the vector reset
		if(!vectorCount)
			return;
		List<Lanes> tuples(vectorCount);
		for(U32 i = 0; i < vectorCount * laneCount; ++i)
			tuples[i / laneCount].push_back(terms[i]);
		// scalar steps removed minus vector ops added. The fold is a shuffle and an op per round, then the extract
		I32 removed = (I32)(vectorCount * (laneCount - 1));
		I32 gain = removed - 2 * (I32)(laneCount / 2) - 1;
		vectors.clear();
		vectorsOf = Map<const Node*, List<U32>>();
		List<Node*> termVectors = tryPack(tuples, std::move(steps), gain);
		if(termVectors.empty())
			return;
		Node* result = foldLanes(op, termVectors, type);
		for(U32 i = vectorCount * laneCount; i < terms.size(); ++i)
			result = fn->binary(op, result, terms[i]);
		root->replaceAllUsesWith(result);
		++packedReductions;
	}

	List<Node*> SlpPass::flatten(Node* root, Set<const Node*>& steps) {
		Opcode op = root->getOpcode();
		Type* type = root->getType();
		List<Node*> terms, todo = {root};
		while(!todo.empty()) {
			Node* n = todo.back();
			todo.pop_back();
			B32 sameOp = n->getOpcode() == op && n->getType() == type;
			B32 isStep = sameOp && (n == root || n->getUsers().size() == 1);
			// a step adds two terms
			B32 room = terms.size() + todo.size() + 2 <= kMaxTerms;
			if(isStep && room) {
				steps.insert(n);
				todo.insert(todo.end(), {n->getInput(0), n->getInput(1)});
			} else
				terms.push_back(n);
		}
		return {terms.rbegin(), terms.rend()};
	}

	// the vectors combined by op, then their lanes folded by shuffles into lane 0
	Node* SlpPass::foldLanes(Opcode op, const List<Node*>& termVectors, Type* scalarType) {
		Node* folded = termVectors[0];
		for(U32 i = 1; i < termVectors.size(); ++i)
			folded = fn->binary(op, folded, termVectors[i]);
		Type* vecType = folded->getType();
		// kFoldSel[lanes / 4] holds the shuffle of each round, nibble i is the source of lane i
		constexpr U16 kFoldSel[2][2] = {{0x01}, {0x1032, 0x2301}};
		for(U16 sel : kFoldSel[vecType->getVecLanes() / 4]) {
			if(!sel) // 0 pads the 2-lane row
				continue;
			Node* shuffle = fn->create<ShuffleNode>(vecType, folded, folded, sel);
			folded = fn->binary(op, folded, shuffle);
		}
		return fn->create<ExtractNode>(scalarType, folded, 0);
	}

	List<Node*> SlpPass::tryPack(const List<Lanes>& tuples, Set<const Node*> dead, I32 gain) {
		packStart = (U32)vectors.size();
		budget = kMaxOps * (U32)tuples.size();
		List<Node*> roots;
		for(const Lanes& tuple : tuples)
			roots.push_back(vectorize(tuple));
		// users before their operands
		for(U32 i = (U32)vectors.size(); i-- > packStart;) {
			for(Node* n : vectors[i].lanes)
				gain += markDead(n, dead);
			gain -= vectors[i].cost;
		}
		B32 allPacked = !std::count(roots.begin(), roots.end(), nullptr);
		if(gain >= kMinProfit && allPacked)
			return roots;
		rollback(packStart);
		return {};
	}

	B32 SlpPass::markDead(Node* n, Set<const Node*>& dead) {
		if(n->getUsers().size() > kMaxOps)
			return false;
		for(U32 i = 0; i < n->getUsers().size(); ++i) {
			Node* u = n->getUsers()[i];
			B32 valueSlot = n->getUserSlot(i) == detail::kValueInput;
			B32 copy = isa<LoadNode>(n) && isa<StoreNode>(u) && valueSlot;
			if(!copy && !dead.count(u))
				return false;
		}
		return dead.insert(n).second; // counted once
	}

	// the vector whose lane i is lanes[i], null when the lanes cannot be one vector
	Node* SlpPass::vectorize(const Lanes& lanes) {
		const List<U32>& sameLane0 = vectorsOf[lanes[0]];
		for(U32 k = (U32)sameLane0.size(); k-- > 0;) { // the newest first
			const Vector& vec = vectors[sameLane0[k]];
			const Lanes& known = vec.lanes;
			if(!std::equal(lanes.begin(), lanes.end(), known.begin(), known.end()))
				continue;
			if(sameLane0[k] >= packStart)
				return vec.node;
			// an earlier pack's vector joins this pack once, so its lanes count as dead here too
			return addVector(lanes, vec.node, 0);
		}
		U32 bytes = lanes.size() * laneType(lanes[0])->byteSize(ptrBytes);
		if(bytes > kVecBytes || bytes < 4)
			return nullptr;
		for(Node* lane : lanes)
			if(lane->getType() != lanes[0]->getType())
				return nullptr;
		if(Node* load = wideLoad(lanes))
			return load;
		U32 vectorsBefore = (U32)vectors.size();
		if(Node* op = vectorOp(lanes))
			return op;
		rollback(vectorsBefore); // the operand vectors of a failed op
		return gather(lanes);
	}

	Node* SlpPass::wideLoad(const Lanes& lanes) {
		LoadNode* lane0 = dyn_cast<LoadNode>(lanes[0]);
		U16 sel = 0;
		Lanes ordered; // the window in address order
		if(!lane0 || !loadSlots(lanes, lane0, sel, ordered))
			return nullptr;
		Type* scalarType = lane0->getType();
		U32 size = scalarType->byteSize(ptrBytes);
		Type* type = fn->types().getVec(scalarType, lanes.size());
		B32 oneAddress = sel == 0; // every lane at element 0
		B32 permuted = !std::equal(lanes.begin(), lanes.end(), ordered.begin());
		if(size < 4 && (oneAddress || permuted))
			return nullptr; // x86 splats and shuffles only dword and qword lanes
		Node* control = lane0->getControl();
		Node* state = lane0->getMemory();
		// a new load, so the scheduler puts it next to its use
		if(oneAddress) {
			Node* pointer = lane0->getPointer();
			Node* one = fn->create<LoadNode>(scalarType, control, state, pointer);
			return addVector(lanes, fn->create<SplatNode>(type, one), 1);
		}
		// of the window start
		Node* pointer = cast<LoadNode>(ordered[0])->getPointer();
		if(forwardStalls(state, pointer, size * lanes.size()))
			return nullptr;
		// a shuffle of the wide load of the window, which passes the same checks
		if(permuted) {
			Node* wide = wideLoad(ordered);
			Node* shuffle = fn->create<ShuffleNode>(type, wide, wide, sel);
			return addVector(lanes, shuffle, 1);
		}
		// one per address, type, state and control in a stretch
		for(U32 i : vectorsOf[pointer]) {
			LoadNode* load = dyn_cast<LoadNode>(vectors[i].node);
			if(!load || load->getPointer() != pointer || load->getType() != type)
				continue;
			if(load->getMemory() == state && load->getControl() == control)
				return addVector(lanes, load, 0); // so its lanes count as dead
		}
		Node* wide = fn->create<LoadNode>(type, control, state, pointer);
		return addVector(lanes, wide, 1);
	}

	B32 SlpPass::loadSlots(const Lanes& lanes, LoadNode* lane0, U16& sel, Lanes& ordered) {
		U32 count = lanes.size(), frame = 2 * count - 1, start = count - 1;
		U32 slots[kMaxLanes]; // the element of each lane
		// by elementOf, lanes win over other loads of their bytes
		Node* loads[2 * kMaxLanes - 1] = {};
		for(U32 k = 0; k < count; ++k) {
			slots[k] = elementOf(lanes[k], lane0, count);
			if(slots[k] >= frame)
				return false;
			start = std::min(start, slots[k]);
			loads[slots[k]] = lanes[k];
		}
		U32 holes = (U32)std::count(loads + start, loads + start + count, nullptr);
		B32 oneAddress = holes == count - 1;
		if(holes && !oneAddress)
			for(Node* user : lane0->getMemory()->getUsers()) {
				U32 at = elementOf(user, lane0, count);
				if(at < frame && !loads[at] && user->hasUsers())
					loads[at] = user;
			}
		for(; !oneAddress; --start) {
			if(!std::count(loads + start, loads + start + count, nullptr))
				break;
			if(!start)
				return false; // no full window
		}
		ordered.assign(loads + start, loads + start + count);
		for(U32 k = 0; k < count; ++k)
			sel |= (U16)((slots[k] - start) << (ShuffleNode::kSourceBits * k));
		// no lane above the window
		return *std::max_element(slots, slots + count) - start < count;
	}

	U32 SlpPass::elementOf(Node* n, LoadNode* lane0, U32 count) {
		U32 frame = 2 * count - 1;
		LoadNode* load = dyn_cast<LoadNode>(n);
		if(!load)
			return frame;
		Addr addr = addrs.get(load->getPointer());
		Addr addr0 = addrs.get(lane0->getPointer());
		if(!addr.group || addr.group != addr0.group)
			return frame;
		if(load->isVolatile() || load->getType() != lane0->getType())
			return frame;
		B32 sameState = load->getMemory() == lane0->getMemory();
		if(!sameState || load->getControl() != lane0->getControl())
			return frame;
		U32 size = lane0->getType()->byteSize(ptrBytes);
		U64 above = (U64)(addr.offset - addr0.offset) + (count - 1) * size;
		if(above % size || above / size >= frame)
			return frame;
		return (U32)(above / size);
	}

	B32 SlpPass::forwardStalls(Node* state, Node* pointer, U32 bytes) {
		Addr loaded = addrs.get(pointer);
		for(U32 hops = 0; hops < kMaxHops; ++hops) {
			PhiNode* phi = dyn_cast<PhiNode>(state);
			if(phi && phi->getRegion()->isLoopHeader()) {
				state = phi->getValue(0); // the first iteration reads the entry state
				continue;
			}
			StoreNode* st = dyn_cast<StoreNode>(state);
			if(!st)
				return callWrites(state, loaded.object);
			Addr addr = addrs.get(st->getPointer());
			U32 size = st->getValue()->getType()->byteSize(ptrBytes);
			B32 startsBelow = addr.offset <= loaded.offset;
			B32 endsAbove = addr.offset + size >= loaded.offset + bytes;
			B32 holdsAll = startsBelow && endsAbove;
			// an unproven overlap with another group is no stall
			if(!disjoint(loaded, bytes, addr, size))
				return addr.group == loaded.group && !holdsAll;
			state = st->getMemory();
		}
		return false;
	}

	B32 SlpPass::callWrites(Node* state, U32 object) {
		ProjNode* proj = dyn_cast<ProjNode>(state);
		CallNode* call = proj ? dyn_cast<CallNode>(proj->getProducer()) : nullptr;
		if(!call || !object || storedObjects.count(object))
			return false;
		for(U32 i = 0; i < call->getArgCount(); ++i) {
			Node* arg = call->getArg(i);
			if(arg->getType()->isPtr() && addrs.get(arg).object == object)
				return true;
		}
		return false;
	}

	Node* SlpPass::vectorOp(const Lanes& lanes) {
		Node* lane0 = lanes[0];
		Opcode op = lane0->getOpcode();
		Opcode alt = op; // the other opcode of a blend
		for(Node* lane : lanes)
			if(lane->getOpcode() != op)
				alt = lane->getOpcode();
		B32 shift = isShiftOpcode(op);
		B32 binary = isBinaryOpcode(op) && isBinaryOpcode(alt);
		B32 blendable = binary && !shift && !isShiftOpcode(alt);
		B32 fits = budget && (alt == op || blendable);
		for(Node* lane : lanes) {
			B32 sameOp = lane->getOpcode() == op || lane->getOpcode() == alt;
			fits = fits && sameOp && hasRule(lane);
			// SSE shifts every lane by one amount
			fits = fits && (!shift || lane->getInput(1) == lane0->getInput(1));
		}
		if(!fits)
			return nullptr;
		// SSE zeroes the lanes of a shift past the width
		const ConstantNode* amount = nullptr;
		if(shift)
			amount = dyn_cast<ConstantNode>(lane0->getInput(1));
		if(amount && (U64)amount->getValue() >= lane0->getType()->getIntWidth())
			return nullptr;
		--budget;
		// a shift amount stays scalar
		Node* inputs[3] = {nullptr, shift ? lane0->getInput(1) : nullptr};
		U32 operandCount = shift ? 1 : (U32)getOpcodeInfo(op).minInputs;
		Type* type = fn->types().getVec(laneType(lane0), lanes.size());
		for(U32 k = 0; k < operandCount; ++k) {
			Lanes operands; // input k of each lane
			for(Node* lane : lanes)
				operands.push_back(lane->getInput(k));
			inputs[k] = vectorize(operands);
			if(!inputs[k])
				return nullptr;
			if(op == Opcode::Select && inputs[0]->getType() != type)
				return nullptr; // blendv needs a mask of the lane type
		}
		Node* node = createArith(*fn, op, type, inputs);
		if(alt == op) {
			I32 variableAmount = shift && !amount; // a variable amount moves to xmm
			return addVector(lanes, node, 1 + variableAmount);
		}
		return blend(lanes, node, createArith(*fn, alt, type, inputs));
	}

	// lane i from other when it has other's opcode
	Node* SlpPass::blend(const Lanes& lanes, Node* node, Node* other) {
		U16 sel = 0;
		B32 addSub = true; // the lanes subtract and add in turn, one addsubps
		for(U32 i = 0; i < lanes.size(); ++i) {
			Opcode laneOp = lanes[i]->getOpcode();
			U32 source = i + lanes.size() * (laneOp == other->getOpcode());
			sel |= (U16)(source << (ShuffleNode::kSourceBits * i));
			addSub &= laneOp == (i % 2 ? Opcode::FAdd : Opcode::FSub);
		}
		Node* shuffle = fn->create<ShuffleNode>(node->getType(), node, other, sel);
		// addsubps, else two ops and a blend
		return addVector(lanes, shuffle, addSub ? 1 : 3);
	}

	// a rule in kLaneRules fits the opcode and types of lane
	B32 SlpPass::hasRule(const Node* lane) {
		Opcode op = lane->getOpcode();
		const Node* typed = isCompareOpcode(op) ? lane->getInput(0) : lane;
		U8 laneBits = typeBits(typed->getType());
		for(const detail::LaneRule& rule : detail::kLaneRules) {
			if(op < rule.first || op > rule.last || !(laneBits & rule.laneTypes))
				continue;
			U8 inputBits = typeBits(lane->getInput(0)->getType());
			return !rule.inputTypes || (inputBits & rule.inputTypes);
		}
		return false;
	}

	// a compare lane is a mask as wide as its operands
	Type* SlpPass::laneType(const Node* lane) {
		if(!isCompareOpcode(lane->getOpcode()))
			return lane->getType();
		U32 bytes = lane->getInput(0)->getType()->byteSize(ptrBytes);
		return fn->types().getInt(8 * bytes);
	}

	Node* SlpPass::gather(const Lanes& lanes) {
		Node* lane0 = lanes[0];
		if(!(typeBits(lane0->getType()) & detail::kElem))
			return nullptr;
		Type* type = fn->types().getVec(lane0->getType(), lanes.size());
		U32 copies = (U32)std::count(lanes.begin(), lanes.end(), lane0);
		if(!isa<ConstantNode>(lane0) && copies == lanes.size())
			return addVector(lanes, fn->create<SplatNode>(type, lane0), 1);
		I32 variables = 0;
		B32 poolLoad = isa<ConstantNode>(lane0);
		for(Node* n : lanes) {
			const ConstantNode* c = dyn_cast<ConstantNode>(n);
			variables += !c;
			poolLoad |= c && c->getValue() != 0;
		}
		List<Node*> scalars(lanes.begin(), lanes.end());
		Node* pack = fn->create<PackNode>(type, scalars);
		return addVector(lanes, pack, variables + poolLoad);
	}

	Node* SlpPass::addVector(const Lanes& lanes, Node* node, I32 cost) {
		U32 index = (U32)vectors.size();
		vectorsOf[lanes[0]].push_back(index);
		// a wide load is also found by its pointer
		if(LoadNode* load = dyn_cast<LoadNode>(node))
			vectorsOf[load->getPointer()].push_back(index);
		vectors.push_back({lanes, node, cost});
		return node;
	}

	void SlpPass::rollback(U32 keep) {
		for(U32 i = (U32)vectors.size(); i-- > keep;) {
			vectorsOf[vectors[i].lanes[0]].pop_back();
			if(LoadNode* load = dyn_cast<LoadNode>(vectors[i].node))
				vectorsOf[load->getPointer()].pop_back();
			if(vectors[i].cost)
				vectors[i].node->clearInputs();
		}
		vectors.resize(keep);
	}
} // namespace rat
