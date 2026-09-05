// vector-tree construction (the Packer)

#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	using namespace slp;

	SlpPackPass::Packer::Packer(Slp& drv, Node* memIn, const RefinedAddr* windowKey)
	: drv(drv),
		fn(drv.fn),
		memIn(memIn),
		windowKey(windowKey) {}

	// each inner store maps to the lane offsets stored before it
	void SlpPackPass::Packer::collectRun(const Segment& seg, U32 begin, U32 count) {
		List<I64> written;
		for(U32 j = 0; j < count; ++j) {
			const StoreInfo& si = seg[begin + j];
			runStores.insert(si.store);
			if(j + 1 == count)
				break;
			written.push_back(si.key.constant);
			interWritten.emplace(si.store, written);
			for(LoadNode* l : si.observers)
				observers.insert(l);
		}
	}

	void SlpPackPass::Packer::addGuard(const RefinedAddr& k, Node* lane0Ptr, U32 bytes) {
		String sig = groupSig(k);
		for(GuardGroup& g : guardGroups)
			if(g.sig == sig) {
				if(k.constant < g.minC) {
					g.minC = k.constant;
					g.ptr = lane0Ptr;
				}
				g.maxC = std::max(g.maxC, k.constant + (I64)bytes);
				return;
			}
		guardGroups.push_back({std::move(sig), lane0Ptr, k.constant, k.constant + (I64)bytes});
	}

	// rewrite same-group wide-access pointers as anchor + byte delta so lowering
	// folds a displacement (window formation already trusts the refined deltas)
	Node* SlpPackPass::Packer::anchorPtr(Node* ptr, const RefinedAddr& k) {
		if(!k.valid())
			return ptr;
		auto [it, inserted] = drv.addrAnchors.try_emplace(groupSig(k), ptr, k.constant);
		auto [anchor, anchorC] = it->second;
		if(inserted)
			return ptr;
		I64 delta = k.constant - anchorC;
		if(delta == 0)
			return anchor;
		Node* off = fn.create<ConstantNode>(fn.types().getInt(64), delta);
		return fn.create<BinaryNode>(Opcode::Add, ptr->getType(), anchor, off);
	}

	B32 SlpPackPass::Packer::coneTouchesObserver(const Node* n) const {
		if(observers.empty())
			return false;
		List<const Node*> cone;
		if(!dataCone(n, 64, cone))
			return true; // unbounded cone: be conservative
		for(const Node* c : cone)
			if(observers.count(c))
				return true;
		return false;
	}

	String SlpPackPass::Packer::tupleKey(const List<Node*>& lanes) {
		String k;
		k.reserve(lanes.size() * 10);
		for(Node* n : lanes) {
			k += std::to_string(n->getId());
			k.push_back(',');
		}
		return k;
	}

	Node* SlpPackPass::Packer::packTuple(const List<Node*>& lanes, Type* elemTy, U32 depth) {
		Type* vecTy = fn.types().getVec(elemTy, (U32)lanes.size());

		// splat: every lane is the same node
		B32 allSame = true;
		for(Node* n : lanes)
			if(n != lanes[0]) {
				allSame = false;
				break;
			}
		if(allSame && !isa<ConstantNode>(lanes[0]) && !coneTouchesObserver(lanes[0])) {
			profit -= 1; // one broadcast
			return fn.create<SplatNode>(vecTy, lanes[0]);
		}

		String key = tupleKey(lanes);
		if(auto it = memo.find(key); it != memo.end())
			return it->second;

		Node* built = packTupleUncached(lanes, elemTy, vecTy, depth);
		memo.emplace(std::move(key), built);
		return built;
	}

	// strategies in order
	Node* SlpPackPass::Packer::packTupleUncached(const List<Node*>& lanes,
																							 Type* elemTy,
																							 Type* vecTy,
																							 U32 depth) {
		U32 w = (U32)lanes.size();

		// all-constant lanes
		B32 allConst = true;
		for(Node* n : lanes)
			if(!isa<ConstantNode>(n)) {
				allConst = false;
				break;
			}
		if(allConst) {
			profit -= 1;
			return fn.create<PackNode>(vecTy, lanes);
		}

		// adjacent loads: a single wide load
		if(Node* wide = packLoads(lanes, elemTy, vecTy))
			return wide;
		if(dead)
			return nullptr;

		// isomorphic binary lanes: one vector op over the two recursed operand tuples
		if(depth < kMaxDepth) {
			if(Node* vbin = packBinaryLanes(lanes, elemTy, vecTy, depth))
				return vbin;
			if(dead)
				return nullptr;
		}

		// build the vector from the scalars lane by lane
		for(Node* n : lanes)
			if(coneTouchesObserver(n))
				return nullptr;
		profit -= (I32)w + 1;
		return fn.create<PackNode>(vecTy, lanes);
	}

	// isomorphic binary lanes, recurse into the two operand tuples and rebuild the op
	Node* SlpPackPass::Packer::packBinaryLanes(const List<Node*>& lanes,
																						 Type* elemTy,
																						 Type* vecTy,
																						 U32 depth) {
		U32 w = (U32)lanes.size();
		Opcode op = lanes[0]->getOpcode();
		for(Node* n : lanes)
			if(!isa<BinaryNode>(n) || n->getOpcode() != op || n->getType() != elemTy)
				return nullptr;
		if(!packableBinary(op, elemTy) || (op == Opcode::Mul && !drv.sse41))
			return nullptr;

		List<Node*> ls, rs;
		BinaryNode* b0 = cast<BinaryNode>(lanes[0]);
		ls.push_back(b0->getLHS());
		rs.push_back(b0->getRHS());
		// commutative lanes orient their operands by shape key
		U64 sl0 = drv.shapes(b0->getLHS()), sr0 = drv.shapes(b0->getRHS());
		for(U32 i = 1; i < w; ++i) {
			BinaryNode* b = cast<BinaryNode>(lanes[i]);
			Node* l = b->getLHS();
			Node* r = b->getRHS();
			if(b->isCommutative() && sl0 != sr0 && !shapesDisabled()) {
				U64 sl = drv.shapes(l), sr = drv.shapes(r);
				if(sl != sl0 && sr == sl0 && sl == sr0)
					std::swap(l, r);
			}
			ls.push_back(l);
			rs.push_back(r);
		}

		Node* lv = packTuple(ls, elemTy, depth + 1);
		Node* rv = packTuple(rs, elemTy, depth + 1);
		if(!lv || !rv) {
			dead = true; // matched, but an operand tuple could not be built
			return nullptr;
		}
		profit += (I32)w - 1;
		++interior;
		return fn.create<BinaryNode>(op, vecTy, lv, rv);
	}

	Node* SlpPackPass::Packer::packLoads(const List<Node*>& lanes, Type* elemTy, Type* vecTy) {
		U32 w = (U32)lanes.size();
		U32 esz = elemTy->byteSize(drv.ptrBytes);
		for(Node* n : lanes)
			if(!isa<LoadNode>(n) || n->getType() != elemTy)
				return nullptr;
		LoadNode* first = cast<LoadNode>(lanes[0]);
		RefinedAddr k0 = refineAddr(first->getPointer(), esz);
		if(!k0.valid())
			return nullptr;
		B32 sharedState = true;
		B32 adjacent = true, equal = true;
		for(U32 i = 1; i < w; ++i) {
			LoadNode* l = cast<LoadNode>(lanes[i]);
			if(l->getControl() != first->getControl())
				return nullptr;
			sharedState &= l->getMemory() == first->getMemory();
			RefinedAddr k = refineAddr(l->getPointer(), esz);
			if(!k.valid() || !k.sameGroup(k0))
				return nullptr;
			adjacent &= k.constant == k0.constant + (I64)(i * esz);
			equal &= k.constant == k0.constant;
		}
		if(!adjacent && !equal)
			return nullptr;

		// A same-group load range that straddles the store window without matching it exactly is a store-forward trap
		if(windowKey && !equal && k0.sameGroup(*windowKey)) {
			I64 sLo = windowKey->constant, sHi = sLo + (I64)(w * esz);
			I64 lLo = k0.constant, lHi = lLo + (I64)(w * esz);
			if(lLo != sLo && lLo < sHi && sLo < lHi) {
				++drv.stats.rejectedOverlap;
				dead = true;
				return nullptr;
			}
		}

		// all lanes read one pre-window state: a single wide load (or splat)
		if(sharedState && !interWritten.count(first->getMemory()))
			return packWideOrSplat(first->getMemory(), first, k0, elemTy, vecTy, w, equal);

		// lanes read inner window states: hoist them to memIn unless one reads a
		// lane stored earlier in the window (never reached by reductions, their
		// interWritten is empty)
		for(U32 i = 0; i < w; ++i) {
			LoadNode* l = cast<LoadNode>(lanes[i]);
			Node* m = l->getMemory();
			if(m == memIn)
				continue;
			auto it = interWritten.find(m);
			if(it == interWritten.end())
				return nullptr; // some other state, not this window's business
			if(k0.sameGroup(*windowKey)) {
				I64 laneOff = 0;
				if(adjacent)
					laneOff = (I64)(i * esz);
				// this lane reads [c, c + esz), a lane stored before it covers [written, written + ssz).
				// any overlap, not just an exact hit, means the scalar sees a freshly stored byte
				I64 c = k0.constant + laneOff;
				I64 ssz = (I64)windowKey->size;
				for(I64 written : it->second)
					if(written < c + (I64)esz && c < written + ssz) {
						dead = true; // scalar reads a freshly stored lane
						return nullptr;
					}
			}
		}

		if(!k0.sameGroup(*windowKey)) {
			U32 guardBytes = w * esz;
			if(equal)
				guardBytes = esz;
			if(!provablyDisjoint(k0, guardBytes, *windowKey, w * windowKey->size))
				addGuard(k0, first->getPointer(), guardBytes);
		}

		return packWideOrSplat(memIn, first, k0, elemTy, vecTy, w, equal);
	}

	// materialize a whole-vector load, or a splat when every lane hits one address
	Node* SlpPackPass::Packer::packWideOrSplat(Node* mem,
																						 LoadNode* first,
																						 const RefinedAddr& k0,
																						 Type* elemTy,
																						 Type* vecTy,
																						 U32 w,
																						 B32 equal) {
		Node* ptr = anchorPtr(first->getPointer(), k0);
		if(equal) {
			profit += (I32)w - 2; // w scalar loads become one load and one broadcast
			Node* ld = fn.create<LoadNode>(elemTy, first->getControl(), mem, ptr);
			Node* sp = fn.create<SplatNode>(vecTy, ld);
			splatLoads.push_back({sp, cast<LoadNode>(ld)});
			madeLoads.insert(ld);
			return sp;
		}
		profit += (I32)w - 1;
		++interior;
		Node* wide = fn.create<LoadNode>(vecTy, first->getControl(), mem, ptr);
		madeLoads.insert(wide);
		return wide;
	}

	// splat reloads at consecutive addresses fold into one wide load
	// each broadcast then picks its lane in-register instead of touching memory
	void SlpPackPass::Packer::coalesceSplats() {
		struct Cand {
			String sig; // address group + state + control
			I64 c;
			Node* splat;
			LoadNode* load;
			bool operator<(const Cand& o) const {
				if(sig != o.sig)
					return sig < o.sig;
				return c < o.c;
			}
		};
		List<Cand> cs;
		for(auto& [splat, load] : splatLoads) {
			U32 esz = load->getType()->byteSize(drv.ptrBytes);
			RefinedAddr k = refineAddr(load->getPointer(), esz);
			if(!supportedEsz(esz) || !k.valid())
				continue;
			// key splats by address group + memory state + control so only truly
			// coincident reloads coalesce
			String sig = groupSig(k);
			sig += '@' + std::to_string(load->getMemory()->getId());
			sig += '@' + std::to_string(load->getControl()->getId());
			cs.push_back({std::move(sig), k.constant, splat, load});
		}
		std::sort(cs.begin(), cs.end());
		for(U32 i = 0; i < cs.size();) {
			U32 esz = cs[i].load->getType()->byteSize(drv.ptrBytes);
			U32 w = laneCountFor(esz);
			Type* vecTy = fn.types().getVec(cs[i].load->getType(), w);
			B32 run = i + w <= (U32)cs.size() && cs[i].splat->getType() == vecTy;
			for(U32 j = 1; run && j < w; ++j) {
				B32 sameSig = cs[i + j].sig == cs[i].sig;
				B32 contiguous = cs[i + j].c == cs[i].c + (I64)(j * esz);
				B32 sameVec = cs[i + j].splat->getType() == vecTy;
				run = sameSig && contiguous && sameVec;
			}
			if(!run) {
				++i;
				continue;
			}
			Node* wide = fn.create<LoadNode>(
					vecTy, cs[i].load->getControl(), cs[i].load->getMemory(), cs[i].load->getPointer());
			for(U32 j = 0; j < w; ++j) {
				// broadcast lane j: pshufd selector for 4-byte lanes, unpack hi/lo for 8-byte
				U8 sel;
				if(esz == 4)
					sel = (U8)(j * 0x55);
				else if(j)
					sel = 0xee;
				else
					sel = 0x44;
				cs[i + j].splat->replaceAllUsesWith(fn.create<ShuffleNode>(vecTy, wide, sel));
			}
			i += w;
		}
	}
} // namespace rat
