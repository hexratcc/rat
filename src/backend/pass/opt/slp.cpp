#include "pass/opt/slp.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"
#include "pass/opt/load_canon.h"
#include "target/target.h"

#include <ostream>

namespace rat {
	namespace detail {
		// One bit per scalar type, so a rule can name a set of types
		enum TypeBit : U8 {
			kInt8 = 1,
			kInt16 = 2,
			kInt32 = 4,
			kInt64 = 8,
			kFloat32 = 16,
			kFloat64 = 32,
			kPtr = 64,
			kInt32Int64 = kInt32 | kInt64,
			kFloat = kFloat32 | kFloat64,
			kNarrow = kInt8 | kInt16,
		};

		constexpr U32 kValueInput = 3; // of a store

		// Lane types SSE packs for a range of opcodes, see hasRule
		struct LaneRule {
			Opcode first;
			Opcode last;	 // inclusive, in Opcode order
			U8 laneTypes;	 // 0 for any, compares are i1 so they check inputTypes
			U8 inputTypes; // of input 0, 0 for any
		};

		// clang-format off
		constexpr LaneRule kLaneRules[] = {
			{Opcode::Add,     Opcode::Sub,     kInt32Int64, 0},
			{Opcode::Mul,     Opcode::Mul,     kInt32,      0},                // pmulld, i32 only
			{Opcode::And,     Opcode::LShr,    kInt32Int64, 0},                // logic, shl, lshr
			{Opcode::AShr,    Opcode::AShr,    kInt32,      0},                // psrad, no i64 sar
			{Opcode::FAdd,    Opcode::FDiv,    kFloat,      0},
			{Opcode::FNeg,    Opcode::FAbs,    kFloat,      0},                // fneg, sqrtps/pd, fabs
			{Opcode::Neg,     Opcode::Not,     kInt32Int64, 0},                // psub from 0, pxor of -1
			{Opcode::Eq,      Opcode::Ne,      0,           kInt32Int64},      // pcmpeqd/q
			{Opcode::Slt,     Opcode::Ule,     0,           kInt32},           // pcmpgtd, pminud
			{Opcode::FEq,     Opcode::FGe,     0,           kFloat},           // cmpps/pd
			{Opcode::SExt,    Opcode::ZExt,    kInt32Int64, kNarrow | kInt32}, // pmovsx/zx
			{Opcode::Trunc,   Opcode::Trunc,   kNarrow,     kInt32},           // pshufb
			{Opcode::SIToFP,  Opcode::SIToFP,  kFloat,      kInt32},           // cvtdq2ps/pd
			{Opcode::FPToSI,  Opcode::FPToSI,  kInt32,      kFloat},           // cvttps2dq/pd2dq
			{Opcode::FPExt,   Opcode::FPExt,   kFloat64,    kFloat32},         // cvtps2pd
			{Opcode::FPTrunc, Opcode::FPTrunc, kFloat32,    kFloat64},         // cvtpd2ps
			{Opcode::Select,  Opcode::Select,  kInt32Int64 | kFloat, 0},       // blendv, pminsd, minps
		};
		// clang-format on
	} // namespace detail

	void SlpPass::printStats(const Module&, std::ostream& os) const {
		os << "slp: windows " << packedWindows << " reductions " << packedReductions << "\n";
	}

	// Store windows chain by chain, then reductions. Returns the number of packs
	U32 SlpPass::runOnFunction(Function& function, const TargetInfo& target) {
		fn = &function;
		ptrBytes = target.getPointerSizeInBytes();
		LoadCanonPass::Handoff handoff = LoadCanonPass::takeHandoff(function);
		addrs = std::move(handoff.addrs);
		storedObjects.clear();
		for(const List<StoreNode*>& chain : handoff.chains)
			for(StoreNode* store : chain)
				storedObjects.insert(addrs.get(store->getPointer()).object);
		U32 packsBefore = packedWindows + packedReductions;
		List<B32> packed; // per chain
		for(const List<StoreNode*>& chain : handoff.chains)
			packed.push_back(packChain(chain));
		// Unpacked chains get their loads back, hoisted loads would sit far from their use
		LoadCanonPass::unhoistLoads(handoff.hoists, packed);
		function.eliminateDeadNodes();
		// Walk a copy, packing adds nodes
		for(Node* node : List<Node*>(function.begin(), function.end()))
			packReduction(node);
		// Drop replaced scalars and rejected vectors
		function.eliminateDeadNodes();
		return packedWindows + packedReductions - packsBefore;
	}

	// A stretch is a run of disjoint stores nothing reads in between, so they can reorder.
	// True if any window packed
	B32 SlpPass::packChain(const List<StoreNode*>& chain) {
		U32 windowsBefore = packedWindows;
		List<StoreAccess> stretch;
		for(StoreNode* store : chain) {
			U32 size = store->getValue()->getType()->byteSize(ptrBytes);
			Addr addr = addrs.get(store->getPointer());
			B32 prevUnread = stretch.empty() || stretch.back().store->getUsers().size() == 1;
			B32 joins = prevUnread && stretch.size() < kMaxStretch;
			for(const StoreAccess& member : stretch)
				joins = joins && disjoint(addr, size, member.addr, member.size);
			if(!joins)
				packStretch(stretch);
			stretch.push_back({store, addr, size}); // AddrCache gives every store a group
		}
		packStretch(stretch);
		return packedWindows != windowsBefore;
	}

	// Packs windows in address order and empties the stretch
	void SlpPass::packStretch(List<StoreAccess>& stretch) {
		vectors.clear(); // packs of one stretch share vectors
		std::sort(stretch.begin(), stretch.end(), byAddress);
		for(U32 start = 0; start < stretch.size();) {
			const StoreAccess& first = stretch[start];
			B32 scalar = typeBits(first.store->getValue()->getType()) != 0;
			U32 maxCount = scalar ? kVecBytes / first.size : 1;
			U32 count = 1;
			for(; count < maxCount && start + count < stretch.size(); ++count) {
				const Addr& addr = stretch[start + count].addr;
				if(addr.group != first.addr.group || addr.offset != first.addr.offset + count * first.size)
					break;
			}
			// Round down, lane counts are powers of two
			count = 1u << (63 - countLeadingZeros64(count));
			while(count > 1 && !packWindow(stretch.data() + start, count))
				count /= 2; // retry with the first half
			start += count;
		}
		stretch.clear();
	}

	B32 SlpPass::byAddress(const StoreAccess& a, const StoreAccess& b) {
		return std::pair(a.addr.group, a.addr.offset) < std::pair(b.addr.group, b.addr.offset);
	}

	// Merge the window into one vector store
	B32 SlpPass::packWindow(const StoreAccess* window, U32 count) {
		Lanes values;
		Set<const Node*> stores;
		for(U32 i = 0; i < count; ++i) {
			values.push_back(window[i].store->getValue());
			stores.insert(window[i].store);
		}
		I32 gain = (I32)count - 1; // stores saved
		List<Node*> roots = tryPack({values}, std::move(stores), gain, kMinProfit);
		if(roots.empty())
			return false;
		window[0].store->setInput(detail::kValueInput, roots[0]);
		for(U32 i = 1; i < count; ++i) {
			StoreNode* store = window[i].store;
			store->replaceAllUsesWith(store->getMemory());
			store->clearInputs(); // a store with no control is dead
		}
		++packedWindows;
		return true;
	}

	// Pack the chain that ends at root, then fold the lanes
	void SlpPass::packReduction(Node* root) {
		// min and max reassociate too
		B32 minMax = isMinMax(root) && hasRule(root->getInput(0));
		B32 reassociates = root->isCommutative() || minMax;
		// Roots are i32, i64, f32 or f64, so i1 compares drop out. fsub and fdiv do not commute,
		// folding them would raise flags the scalar code does not, fsub under directed rounding
		U8 bits = typeBits(root->getType()) & (detail::kInt32Int64 | detail::kFloat);
		B32 floatRoot = bits & detail::kFloat;
		// Only a live last step is a root
		if(!reassociates || !bits || !hasRule(root) || !root->hasUsers())
			return;
		for(Node* user : root->getUsers())
			if(sameStep(user, root))
				return;
		Terms steps = {root}; // die with the pack
		if(minMax)
			steps.push_back(root->getInput(0)); // the compare dies with its select
		// A float root packs just its two operands, lane 0 op lane 1 is the scalar op exactly.
		// A 2-lane f32 repeats in the upper lanes, so no extra flags, see repeatsLanes
		U32 firstInput = minMax ? 1 : 0; // input 0 of a select is its condition
		Terms terms = {root->getInput(firstInput), root->getInput(firstInput + 1)};
		if(!floatRoot)
			terms = flatten(root, steps); // an int root packs its whole chain
		else if(terms[0]->getOpcode() != terms[1]->getOpcode() || !hasRule(terms[0]))
			return; // both terms must be the same packable op
		U32 laneCount = floatRoot ? 2 : kVecBytes / root->getType()->byteSize(ptrBytes);
		U32 vectorCount = terms.size() / laneCount;
		// Most roots stop here, before we allocate
		if(!vectorCount)
			return;
		List<Lanes> tuples(vectorCount);
		for(U32 i = 0; i < vectorCount * laneCount; ++i)
			tuples[i / laneCount].push_back(terms[i]);
		// Scalar steps saved minus vector ops, a shuffle and op per fold round, and the extract
		I32 gain = (I32)(vectorCount * (laneCount - 1)) - 2 * (I32)(laneCount / 2) - 1;
		vectors.clear();
		Set<const Node*> dead(steps.begin(), steps.end());
		List<Node*> termVectors = tryPack(tuples, std::move(dead), gain, kMinReduceProfit);
		if(termVectors.empty())
			return;
		// Vectors spill around a call, then only an int reduction saving kMinProfit pays
		B32 paysAcrossCall = !floatRoot && gain >= kMinProfit;
		if(!paysAcrossCall && loadsBeforeCall()) {
			rollback(packStart);
			return;
		}
		root->replaceAllUsesWith(foldLanes(root, termVectors, terms));
		++packedReductions;
	}

	// Terms of the chain in source order. Inner steps and their compares go to steps
	SlpPass::Terms SlpPass::flatten(Node* root, Terms& steps) {
		B32 minMax = isa<SelectNode>(root);
		// An inner step feeds only the next step, a min or max step also the next compare.
		// Counting users is enough, a compare takes the arms of its select, see isMinMax
		U32 stepUsers = minMax ? 2 : 1;
		// min and max chains run through the false arm, it counts as the left operand
		U32 leftInput = minMax ? 2 : 0;
		Terms terms;
		Terms todo = {root->getInput(leftInput), root->getInput(1)};
		while(!todo.empty()) {
			Node* node = todo.back();
			todo.pop_back();
			B32 isStep = sameStep(node, root) && node->getUsers().size() == stepUsers;
			B32 room = terms.size() + todo.size() + 2 <= kMaxTerms; // a step adds two terms
			if(isStep && room) {
				steps.push_back(node);
				if(minMax)
					steps.push_back(node->getInput(0)); // its compare dies with it
				todo.push_back(node->getInput(leftInput));
				todo.push_back(node->getInput(1));
			} else
				terms.push_back(node);
		}
		std::reverse(terms.begin(), terms.end());
		return terms;
	}

	// Combine the vectors, fold them into lane 0, then combine in the leftover scalar terms
	Node* SlpPass::foldLanes(Node* root, const List<Node*>& termVectors, const Terms& terms) {
		Node* folded = termVectors[0];
		for(U32 i = 1; i < termVectors.size(); ++i)
			folded = combine(root, folded, termVectors[i]);
		U32 laneCount = folded->getType()->getVecLanes();
		// Each round combines lane i with lane i ^ half, lane 0 ends up with all of them
		for(U32 half = laneCount / 2; half; half /= 2) {
			U16 sel = 0; // nibble i is the source of lane i
			for(U32 i = 0; i < laneCount; ++i)
				sel |= (U16)((i ^ half) << (ShuffleNode::kSourceBits * i));
			Node* shuffle = fn->create<ShuffleNode>(folded->getType(), folded, folded, sel);
			folded = combine(root, folded, shuffle);
		}
		Node* result = fn->create<ExtractNode>(root->getType(), folded, 0);
		for(U32 i = (U32)termVectors.size() * laneCount; i < terms.size(); ++i)
			result = combine(root, result, terms[i]);
		return result;
	}

	// a op b like root, a compare and select for min and max
	Node* SlpPass::combine(Node* root, Node* a, Node* b) {
		if(!isa<SelectNode>(root))
			return fn->binary(root->getOpcode(), a, b);
		Node* compare = root->getInput(0);
		Type* type = a->getType();
		Type* maskType = compare->getType();
		if(type->isVec())
			maskType = fn->types().getVec(laneType(compare), type->getVecLanes());
		Node* inputs[2] = {a, b};
		if(!picksCompareLhs(root))
			std::swap(inputs[0], inputs[1]);
		Node* mask = createArith(*fn, compare->getOpcode(), maskType, inputs);
		return fn->create<SelectNode>(type, mask, a, b);
	}

	// Selects the smaller or larger operand of a compare only it uses
	B32 SlpPass::isMinMax(const Node* node) {
		if(!isa<SelectNode>(node))
			return false;
		const Node* compare = node->getInput(0);
		Opcode op = compare->getOpcode();
		B32 orders = isFloatOrderOpcode(op) || (op >= Opcode::Slt && op <= Opcode::Ule); // int orders
		return orders && compare->getUsers().size() == 1 &&
					 armsAreOperands(compare, node->getInput(1), node->getInput(2));
	}

	// Same op as root, for min and max also the same compare and direction
	B32 SlpPass::sameStep(const Node* node, const Node* root) {
		if(node->getOpcode() != root->getOpcode() || node->getType() != root->getType())
			return false;
		if(!isa<SelectNode>(root))
			return true;
		B32 sameCompare = node->getInput(0)->getOpcode() == root->getInput(0)->getOpcode();
		return isMinMax(node) && sameCompare && picksCompareLhs(node) == picksCompareLhs(root);
	}

	// A call follows a wide load of the pack. The fold may land after it, and SysV saves no xmm
	// register, so the vectors spill
	B32 SlpPass::loadsBeforeCall() {
		// Load states, then the stores and phis after them, each once
		List<Node*> states;
		for(const Vector& vec : vectors) {
			LoadNode* load = dyn_cast<LoadNode>(vec.node);
			if(load && !std::count(states.begin(), states.end(), load->getMemory()))
				states.push_back(load->getMemory());
		}
		for(U32 i = 0; i < states.size() && i < kMaxHops; ++i)
			for(Node* user : states[i]->getUsers()) {
				if(isa<CallNode>(user) || isa<AsmNode>(user))
					return true; // treat asm as a call
				B32 walks = isa<StoreNode>(user) || isa<PhiNode>(user);
				if(walks && !std::count(states.begin(), states.end(), user))
					states.push_back(user);
			}
		return states.size() > kMaxHops; // too far, assume a call
	}

	// One vector per tuple, empty if a tuple fails or gain stays below minProfit. gain comes in
	// as the ops saved outside the vectors and leaves as the pack total
	List<Node*>
	SlpPass::tryPack(const List<Lanes>& tuples, Set<const Node*> dead, I32& gain, I32 minProfit) {
		packStart = (U32)vectors.size();
		budget = kMaxOps * (U32)tuples.size();
		List<Node*> roots;
		for(const Lanes& tuple : tuples)
			roots.push_back(vectorize(tuple));
		// Newest first, so users die before their operands
		for(U32 i = (U32)vectors.size(); i-- > packStart;) {
			for(Node* lane : vectors[i].lanes)
				gain += markDead(lane, dead);
			gain -= vectors[i].cost;
		}
		if(gain >= minProfit && !std::count(roots.begin(), roots.end(), nullptr))
			return roots;
		rollback(packStart);
		return {};
	}

	// lane dies when every user dies, or it is a load and the user stores it
	B32 SlpPass::markDead(Node* lane, Set<const Node*>& dead) {
		if(lane->getUsers().size() > kMaxUsers)
			return false;
		for(U32 i = 0; i < lane->getUsers().size(); ++i) {
			Node* user = lane->getUsers()[i];
			B32 storesIt = isa<StoreNode>(user) && lane->getUserSlot(i) == detail::kValueInput;
			if(!dead.count(user) && !(isa<LoadNode>(lane) && storesIt))
				return false;
		}
		return dead.insert(lane).second; // counted once
	}

	// The vector of the lanes, null if they do not pack
	Node* SlpPass::vectorize(const Lanes& lanes) {
		for(U32 i = (U32)vectors.size(); i-- > 0;) { // newest first
			const Vector& vec = vectors[i];
			if(!std::equal(lanes.begin(), lanes.end(), vec.lanes.begin(), vec.lanes.end()))
				continue;
			if(i >= packStart)
				return vec.node;
			// Borrow an earlier pack's vector once, so its lanes count as dead here too
			return addVector(lanes, vec.node, 0);
		}
		U32 bytes = lanes.size() * laneType(lanes[0])->byteSize(ptrBytes);
		if(bytes > kVecBytes || bytes < 4)
			return nullptr; // x86 vectors are 4, 8 or 16 bytes, see opWidth
		for(Node* lane : lanes)
			if(lane->getType() != lanes[0]->getType())
				return nullptr;
		if(Node* load = wideLoad(lanes))
			return load;
		U32 vectorsBefore = (U32)vectors.size(); // a failed try drops its operand vectors
		if(Node* narrow = demote(lanes))
			return narrow;
		rollback(vectorsBefore);
		if(Node* op = vectorOp(lanes))
			return op;
		rollback(vectorsBefore);
		return gather(lanes);
	}

	// A wide load, a splat of one load, or a shuffle of one or two wide loads
	Node* SlpPass::wideLoad(const Lanes& lanes) {
		LoadNode* lane0 = dyn_cast<LoadNode>(lanes[0]);
		U16 sel = 0;
		Lanes low;	// first window, empty for a splat
		Lanes high; // second window, may be empty
		if(!lane0 || !loadWindows(lanes, lane0, sel, low, high))
			return nullptr;
		Type* scalarType = lane0->getType();
		U32 size = scalarType->byteSize(ptrBytes);
		Type* type = fn->types().getVec(scalarType, lanes.size());
		B32 inOrder = std::equal(lanes.begin(), lanes.end(), low.begin(), low.end());
		if(size < 4 && !inOrder)
			return nullptr; // x86 splats and shuffles only dword and qword lanes
		if(!high.empty() && size * lanes.size() != kVecBytes)
			return nullptr; // two windows shuffle by one shufps, only for a full vector
		// A new load, so the scheduler can put it next to its use
		if(low.empty()) {
			Node* state = lane0->getMemory();
			Node* one = fn->create<LoadNode>(scalarType, lane0->getControl(), state, lane0->getPointer());
			return addVector(lanes, fn->create<SplatNode>(type, one), 1);
		}
		if(forwardStalls(cast<LoadNode>(low[0]), size * (low.size() + high.size())))
			return nullptr;
		Node* lowLoad = windowLoad(low, type);
		if(inOrder)
			return lowLoad;
		Node* highLoad = high.empty() ? lowLoad : windowLoad(high, type);
		return addVector(lanes, fn->create<ShuffleNode>(type, lowLoad, highLoad, sel), 1);
	}

	// Wide load of a window, one per address, type, state and control in vectors
	Node* SlpPass::windowLoad(const Lanes& window, Type* type) {
		LoadNode* first = cast<LoadNode>(window[0]);
		Node* control = first->getControl();
		Node* state = first->getMemory();
		for(const Vector& vec : vectors) {
			LoadNode* load = dyn_cast<LoadNode>(vec.node);
			B32 sameState = load && load->getMemory() == state && load->getControl() == control;
			if(sameState && load->getPointer() == first->getPointer() && load->getType() == type)
				return addVector(window, load, 0); // so its lanes count as dead
		}
		return addVector(window, fn->create<LoadNode>(type, control, state, first->getPointer()), 1);
	}

	// Find the load windows that hold the lanes, sel shuffles them into lane order
	B32 SlpPass::loadWindows(const Lanes& lanes, LoadNode* lane0, U16& sel, Lanes& low, Lanes& high) {
		U32 laneCount = lanes.size();
		U32 frameSize = 4 * laneCount - 1;	 // fits any two windows around lane0
		U32 slots[kMaxLanes] = {};					 // per lane
		Node* frame[4 * kMaxLanes - 1] = {}; // lanes win over other loads of the same bytes
		for(U32 i = 0; i < laneCount; ++i) {
			slots[i] = slotOf(lanes[i], lane0, frameSize);
			if(slots[i] >= frameSize)
				return false;
			frame[slots[i]] = lanes[i];
		}
		U32 start = *std::min_element(slots, slots + laneCount);
		U32 top = *std::max_element(slots, slots + laneCount);
		if(top == start) // one address, a splat
			return true;
		U32 width = top - start < laneCount ? laneCount : 2 * laneCount; // one window or two
		// Fill holes with other loads of the state, then slide down to the first full window
		if(std::count(frame + start, frame + start + width, nullptr))
			for(Node* user : lane0->getMemory()->getUsers()) {
				U32 slot = slotOf(user, lane0, frameSize);
				if(slot < frameSize && !frame[slot] && user->hasUsers())
					frame[slot] = user;
			}
		for(; std::count(frame + start, frame + start + width, nullptr); --start)
			if(!start)
				return false; // no full window
		for(U32 i = 0; i < laneCount; ++i) {
			U32 source = slots[i] - start; // element in the windows
			// With two windows, low lanes read the first and high lanes the second, so one shufps
			if(width != laneCount && (source >= laneCount) != (2 * i >= laneCount))
				return false;
			// More than four lanes are narrow and never shuffle, see wideLoad
			if(i < ShuffleNode::kMaxLanes)
				sel |= (U16)(source << (ShuffleNode::kSourceBits * i));
		}
		low.assign(frame + start, frame + start + laneCount);
		high.assign(frame + start + laneCount, frame + start + width);
		return top - start < width; // no lane above the windows
	}

	// Frame slot of node with lane0 in the middle, frameSize if no matching load or out of range
	U32 SlpPass::slotOf(Node* node, LoadNode* lane0, U32 frameSize) {
		LoadNode* load = dyn_cast<LoadNode>(node);
		if(!load)
			return frameSize;
		Addr addr = addrs.get(load->getPointer());
		Addr addr0 = addrs.get(lane0->getPointer());
		B32 sameAccess = !load->isVolatile() && load->getType() == lane0->getType() &&
										 load->getMemory() == lane0->getMemory() &&
										 load->getControl() == lane0->getControl() && addr.group == addr0.group;
		I64 size = lane0->getType()->byteSize(ptrBytes);
		I64 distance = addr.offset - addr0.offset;
		I64 slot = distance / size + frameSize / 2;
		if(!sameAccess || distance % size != 0 || slot < 0 || slot >= frameSize)
			return frameSize;
		return (U32)slot;
	}

	// Store forwarding fails if a store above covers only part of the load, or a call writes it
	B32 SlpPass::forwardStalls(LoadNode* first, U32 bytes) {
		Node* state = first->getMemory();
		Addr loaded = addrs.get(first->getPointer());
		for(U32 hops = 0; hops < kMaxHops; ++hops) {
			if(PhiNode* phi = dyn_cast<PhiNode>(state); phi && phi->getRegion()->isLoopHeader()) {
				state = phi->getValue(0); // the first iteration reads the entry state
				continue;
			}
			StoreNode* store = dyn_cast<StoreNode>(state);
			if(!store)
				return callWrites(state, loaded.object);
			Addr addr = addrs.get(store->getPointer());
			U32 size = store->getValue()->getType()->byteSize(ptrBytes);
			B32 holdsAll = addr.offset <= loaded.offset && addr.offset + size >= loaded.offset + bytes;
			// Overlap with another group is unproven, assume no stall
			if(!disjoint(loaded, bytes, addr, size))
				return addr.group == loaded.group && !holdsAll;
			state = store->getMemory();
		}
		return false;
	}

	// state is a call that gets a pointer to object, and no chain store writes object. Likely an
	// out parameter, filled with stores of unknown width
	B32 SlpPass::callWrites(Node* state, U32 object) {
		ProjNode* proj = dyn_cast<ProjNode>(state);
		CallNode* call = proj ? dyn_cast<CallNode>(proj->getProducer()) : nullptr;
		if(!call || !object || storedObjects.count(object))
			return false;
		for(U32 i = 0; i < call->getArgCount(); ++i)
			if(call->getArg(i)->getType()->isPtr() && addrs.get(call->getArg(i)).object == object)
				return true;
		return false;
	}

	// One op on the operand vectors, or a blend of two ops for mixed lanes
	Node* SlpPass::vectorOp(const Lanes& lanes) {
		Node* lane0 = lanes[0];
		Opcode op = lane0->getOpcode();
		Opcode alt = op; // the other op of a blend
		for(Node* lane : lanes)
			if(lane->getOpcode() != op)
				alt = lane->getOpcode();
		// A float blend raises the flags of both ops on every lane, addsubps does not
		if(alt != op && lane0->getType()->isFloat() && !addSubOrder(lanes))
			return addSubShuffled(lanes);
		if(!budget || !opFits(lanes, alt))
			return nullptr;
		--budget;
		B32 shift = isShiftOpcode(op);
		// The shift amount stays scalar
		Node* inputs[3] = {nullptr, shift ? lane0->getInput(1) : nullptr};
		U32 operandCount = shift ? 1 : (U32)getOpcodeInfo(op).minInputs;
		Type* type = fn->types().getVec(laneType(lane0), lanes.size());
		for(U32 k = 0; k < operandCount; ++k) {
			Lanes operands; // input k of each lane
			for(Node* lane : lanes)
				operands.push_back(alt == op ? matchedInput(lane, lane0, k) : lane->getInput(k));
			inputs[k] = vectorize(operands);
			if(!inputs[k])
				return nullptr;
			if(op == Opcode::Select &&
				 inputs[0]->getType()->byteSize(ptrBytes) != type->byteSize(ptrBytes))
				return nullptr; // blendv needs a mask as wide as the lanes
		}
		if(op == Opcode::Neg || op == Opcode::Not)
			return addVector(lanes, negOrNot(op, type, inputs[0]), 2);
		Node* node = createArith(*fn, op, type, inputs);
		if(alt != op)
			return blend(lanes, node, createArith(*fn, alt, type, inputs));
		I32 cost = 1;
		if(shift && !isa<ConstantNode>(inputs[1]))
			++cost; // a variable amount moves to xmm
		// pshufb loads a mask. Scalar truncs are free, so their dead lanes save nothing
		if(op == Opcode::Trunc)
			cost += 1 + (I32)lanes.size();
		return addVector(lanes, node, cost);
	}

	// The lanes fit the SSE op of lane 0, or its blend with alt
	B32 SlpPass::opFits(const Lanes& lanes, Opcode alt) {
		Node* lane0 = lanes[0];
		Opcode op = lane0->getOpcode();
		B32 shift = isShiftOpcode(op);
		B32 blendable = isBinaryOpcode(op) && isBinaryOpcode(alt) && !shift && !isShiftOpcode(alt);
		// SSE zeroes lanes shifted past the width
		const ConstantNode* amount = shift ? dyn_cast<ConstantNode>(lane0->getInput(1)) : nullptr;
		B32 pastWidth = amount && (U64)amount->getValue() >= lane0->getType()->getIntWidth();
		if((alt != op && !blendable) || pastWidth)
			return false;
		B32 floatOrder = op >= Opcode::FLt && op <= Opcode::FGe;
		// cmpltps signals on a quiet NaN, our ucomiss does not. Full width compares pack anyway,
		// narrower ones only for a min or max user, which lowers to minps
		B32 signalingOrder =
				floatOrder && lanes.size() * lane0->getInput(0)->getType()->byteSize(ptrBytes) < kVecBytes;
		for(Node* lane : lanes) {
			B32 sameOp = lane->getOpcode() == op || lane->getOpcode() == alt;
			B32 quiet =
					!signalingOrder || (lane->getUsers().size() == 1 && isMinMax(lane->getUsers()[0]));
			// SSE shifts every lane by one amount
			if(!sameOp || !hasRule(lane) || !quiet || (shift && lane->getInput(1) != lane0->getInput(1)))
				return false;
		}
		return true;
	}

	// trunc(op(ext a, ext b)) is op(a, b) on narrow lanes, the low bits of add, sub, mul, logic
	// and shl only depend on the low bits of their inputs
	Node* SlpPass::demote(const Lanes& lanes) {
		Node* lane0 = lanes[0];
		Type* narrowType = lane0->getType();
		B32 narrow = typeBits(narrowType) & detail::kNarrow;
		if(lane0->getOpcode() != Opcode::Trunc || !narrow || !budget)
			return nullptr;
		Node* wide0 = lane0->getInput(0);
		for(Node* lane : lanes)
			if(!demotable(lane, wide0))
				return nullptr;
		--budget;
		Opcode op = wide0->getOpcode();
		B32 shift = isShiftOpcode(op);
		// A right shift of a sext is an ashr on the narrow lanes, of a zext an lshr
		Opcode narrowOp = op;
		if(shift && op != Opcode::Shl)
			narrowOp = wide0->getInput(0)->getOpcode() == Opcode::SExt ? Opcode::AShr : Opcode::LShr;
		Node* inputs[2] = {nullptr, shift ? wide0->getInput(1) : nullptr};
		U32 operandCount = shift ? 1 : 2;
		for(U32 k = 0; k < operandCount; ++k) {
			Lanes operands; // input k of each wide op, narrowed
			for(Node* lane : lanes)
				operands.push_back(narrowOperand(lane->getInput(0)->getInput(k), narrowType));
			if(std::count(operands.begin(), operands.end(), nullptr))
				return nullptr;
			inputs[k] = vectorize(operands);
			if(!inputs[k])
				return nullptr;
		}
		Type* type = fn->types().getVec(narrowType, lanes.size());
		Node* node = createArith(*fn, narrowOp, type, inputs);
		// SSE has no byte shift or multiply, see emitByteShift and emitByteMul
		I32 cost = 1;
		if(narrowType->isInt(8) && shift)
			cost = narrowOp == Opcode::AShr ? 6 : 3;
		if(narrowType->isInt(8) && op == Opcode::Mul)
			cost = 8;
		return addVector(lanes, node, cost);
	}

	// lane truncs an op like wide0 that only the trunc uses, so the op dies too
	B32 SlpPass::demotable(const Node* lane, const Node* wide0) {
		if(lane->getOpcode() != Opcode::Trunc)
			return false;
		const Node* wide = lane->getInput(0);
		Opcode op = wide->getOpcode();
		B32 narrowable =
				(op >= Opcode::Add && op <= Opcode::Mul) || (op >= Opcode::And && op <= Opcode::AShr);
		B32 sameOp = op == wide0->getOpcode() && wide->getType() == wide0->getType();
		if(!narrowable || !sameOp || wide->getUsers().size() != 1)
			return false;
		if(!isShiftOpcode(op))
			return true;
		// One amount for all lanes, a constant below the narrow width, see emitByteShift
		const ConstantNode* amount = dyn_cast<ConstantNode>(wide->getInput(1));
		B32 belowWidth = amount && (U64)amount->getValue() < lane->getType()->getIntWidth();
		if(wide->getInput(1) != wide0->getInput(1) || !belowWidth)
			return false;
		// A right shift needs the same sext or zext as wide0
		Opcode ext = wide->getInput(0)->getOpcode();
		B32 extended = ext == Opcode::SExt || ext == Opcode::ZExt;
		return op == Opcode::Shl || (extended && ext == wide0->getInput(0)->getOpcode());
	}

	// The narrow value that wide extends, or a constant cut to the narrow type
	Node* SlpPass::narrowOperand(Node* wide, Type* narrowType) {
		if(const ConstantNode* constant = dyn_cast<ConstantNode>(wide))
			return fn->constInt(narrowType, signExtend(constant->getValue(), narrowType->getIntWidth()));
		B32 extended = wide->getOpcode() == Opcode::SExt || wide->getOpcode() == Opcode::ZExt;
		if(!extended || wide->getInput(0)->getType() != narrowType)
			return nullptr;
		return wide->getInput(0);
	}

	// Input k of lane, swapped if a commutative lane has lane0's input 0 on the other side
	Node* SlpPass::matchedInput(Node* lane, Node* lane0, U32 k) {
		Node* input0 = lane0->getInput(0);
		B32 swap = lane->isCommutative() && !alike(lane->getInput(0), input0) &&
							 alike(lane->getInput(1), input0);
		return lane->getInput(swap ? 1 - k : k);
	}

	// Same node, or same opcode, and loads also need the same group
	B32 SlpPass::alike(Node* a, Node* b) {
		if(a->getOpcode() != b->getOpcode())
			return false;
		if(a == b || !isa<LoadNode>(a))
			return true;
		U32 group = addrs.get(cast<LoadNode>(a)->getPointer()).group;
		return group == addrs.get(cast<LoadNode>(b)->getPointer()).group;
	}

	// x86 has no packed neg or not, so 0 - x and x ^ -1
	Node* SlpPass::negOrNot(Opcode op, Type* type, Node* operand) {
		Node* constant = fn->constInt(type->getVecElement(), op == Opcode::Not ? -1 : 0);
		Node* pack = fn->create<PackNode>(type, List<Node*>(type->getVecLanes(), constant));
		if(op == Opcode::Neg)
			return fn->binary(Opcode::Sub, pack, operand);
		return fn->binary(Opcode::Xor, operand, pack);
	}

	// Shuffle of node and other, lane i comes from other if it has other's op
	Node* SlpPass::blend(const Lanes& lanes, Node* node, Node* other) {
		U16 sel = 0;
		for(U32 i = 0; i < lanes.size(); ++i) {
			B32 fromOther = lanes[i]->getOpcode() == other->getOpcode();
			U32 source = fromOther ? lanes.size() + i : i;
			sel |= (U16)(source << (ShuffleNode::kSourceBits * i));
		}
		Node* shuffle = fn->create<ShuffleNode>(node->getType(), node, other, sel);
		// addsubps, else two ops and a blend
		return addVector(lanes, shuffle, addSubOrder(lanes) ? 1 : 3);
	}

	// Mixed fadd and fsub lanes go into addsubps order, then shuffle back. Exact, every lane keeps
	// its own op. Needs as many fadds as fsubs
	Node* SlpPass::addSubShuffled(const Lanes& lanes) {
		if(!budget || lanes.size() > ShuffleNode::kMaxLanes)
			return nullptr; // more lanes than one selector holds
		Lanes subsEven = lanes;
		U32 nextAdd = 1;
		U32 nextSub = 0;
		for(Node* lane : lanes) {
			Opcode laneOp = lane->getOpcode();
			U32& slot = laneOp == Opcode::FSub ? nextSub : nextAdd;
			if((laneOp != Opcode::FAdd && laneOp != Opcode::FSub) || slot >= lanes.size())
				return nullptr;
			subsEven[slot] = lane;
			slot += 2;
		}
		--budget;
		// vectorOp, not vectorize, a failed addsubps must not turn into a gather
		Node* inner = vectorOp(subsEven);
		if(!inner)
			return nullptr;
		U16 sel = 0;
		for(U32 i = 0; i < lanes.size(); ++i) {
			U32 source = (U32)(std::find(subsEven.begin(), subsEven.end(), lanes[i]) - subsEven.begin());
			sel |= (U16)(source << (ShuffleNode::kSourceBits * i));
		}
		return addVector(lanes, fn->create<ShuffleNode>(inner->getType(), inner, inner, sel), 1);
	}

	// sub, add, sub, add, like addsubps
	B32 SlpPass::addSubOrder(const Lanes& lanes) {
		for(U32 i = 0; i < lanes.size(); ++i)
			if(lanes[i]->getOpcode() != (i % 2 ? Opcode::FAdd : Opcode::FSub))
				return false;
		return true;
	}

	// 0 for i1 and non-scalar types
	U8 SlpPass::typeBits(const Type* type) {
		using namespace detail;
		if(type->isPtr())
			return kPtr;
		if(type->isFloat())
			return (type->getFloatWidth() == 32) * kFloat32 | (type->getFloatWidth() == 64) * kFloat64;
		return type->isInt(8) * kInt8 | type->isInt(16) * kInt16 | type->isInt(32) * kInt32 |
					 type->isInt(64) * kInt64;
	}

	// Some kLaneRules row covers the opcode and types of lane
	B32 SlpPass::hasRule(const Node* lane) {
		Opcode op = lane->getOpcode();
		U8 laneBits = typeBits(lane->getType());
		for(const detail::LaneRule& rule : detail::kLaneRules)
			if(op >= rule.first && op <= rule.last && (!rule.laneTypes || (laneBits & rule.laneTypes)))
				return !rule.inputTypes || (typeBits(lane->getInput(0)->getType()) & rule.inputTypes);
		return false;
	}

	// A compare lane is a mask as wide as its operands
	Type* SlpPass::laneType(const Node* lane) {
		if(!isCompareOpcode(lane->getOpcode()))
			return lane->getType();
		return fn->types().getInt(8 * lane->getInput(0)->getType()->byteSize(ptrBytes));
	}

	// Splat or pack the lanes. A pack costs an op per variable and maybe a pool load, see emitPack
	Node* SlpPass::gather(const Lanes& lanes) {
		Node* lane0 = lanes[0];
		I32 variables = 0;
		B32 poolLoad = isa<ConstantNode>(lane0);
		for(Node* lane : lanes) {
			const ConstantNode* constant = dyn_cast<ConstantNode>(lane);
			variables += !constant;
			poolLoad |= constant && constant->getValue() != 0;
		}
		// Scalars only. x86 inserts only dwords and qwords, so narrow lanes must be constants
		U8 bits = typeBits(lane0->getType());
		if(!bits || (variables && (bits & detail::kNarrow)))
			return nullptr;
		Type* type = fn->types().getVec(lane0->getType(), lanes.size());
		// The same variable in every lane
		if(variables && (U32)std::count(lanes.begin(), lanes.end(), lane0) == lanes.size())
			return addVector(lanes, fn->create<SplatNode>(type, lane0), 1);
		Node* pack = fn->create<PackNode>(type, List<Node*>(lanes.begin(), lanes.end()));
		return addVector(lanes, pack, variables + poolLoad);
	}

	// cost counts against the pack gain
	Node* SlpPass::addVector(const Lanes& lanes, Node* node, I32 cost) {
		vectors.push_back({lanes, node, cost});
		return node;
	}

	// Drop vectors from keep on. Nodes with a cost are ours, they lose their inputs so they die
	void SlpPass::rollback(U32 keep) {
		for(; vectors.size() > keep; vectors.pop_back())
			if(vectors.back().cost)
				vectors.back().node->clearInputs();
	}
} // namespace rat
