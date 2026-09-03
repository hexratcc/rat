// vector-tree construction (the Packer)

#include "pass/opt/slp/slp_pack.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"

namespace rat {
	using namespace slp;

	SlpPackPass::Packer::Packer(
			Function& fn, U32 ptrBytes, B32 sse41, ShapeHash& shapes, SlpStats& st)
	: fn(fn),
		ptrBytes(ptrBytes),
		sse41(sse41),
		shapes(shapes),
		st(st) {}

	// point the packer at one store window's memory state and observer context
	void SlpPackPass::Packer::bindWindow(Node* memIn,
																			 const RefinedAddr* windowKey,
																			 const Map<const Node*, List<I64>>* interWritten,
																			 const Set<const Node*>* observers,
																			 Map<String, Pair<Node*, I64>>* addrAnchors,
																			 B32 storeWindow) {
		this->memIn = memIn;
		this->windowKey = windowKey;
		this->interWritten = interWritten;
		this->observers = observers;
		this->addrAnchors = addrAnchors;
		this->storeWindow = storeWindow;
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
		if(!addrAnchors || !k.valid())
			return ptr;
		auto [it, inserted] = addrAnchors->try_emplace(groupSig(k), ptr, k.constant);
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
		if(observers->empty())
			return false;
		List<const Node*> cone;
		if(!dataCone(n, 64, cone))
			return true; // unbounded cone: be conservative
		for(const Node* c : cone)
			if(observers->count(c))
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
			++st.packSplat;
			return fn.create<SplatNode>(vecTy, lanes[0]);
		}

		String key = tupleKey(lanes);
		if(auto it = memo.find(key); it != memo.end()) {
			++st.memoHits;
			return it->second;
		}

		Node* built = packTupleUncached(lanes, elemTy, vecTy, depth);
		memo.emplace(std::move(key), built);
		return built;
	}

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
			++st.packConst;
			return fn.create<PackNode>(vecTy, lanes);
		}

		// adjacent loads: a single wide load
		B32 loadFail = false;
		if(Node* wide = packLoads(lanes, elemTy, vecTy, loadFail))
			return wide;
		if(loadFail)
			return nullptr;

		if(depth < kMaxDepth) {
			// isomorphic binary lanes: one vector op over the two recursed operand tuples
			B32 binFail = false;
			if(Node* vbin = packBinaryLanes(lanes, elemTy, vecTy, depth, binFail))
				return vbin;
			if(binFail)
				return nullptr;
		}

		// build the vector from the scalars lane by lane
		for(Node* n : lanes)
			if(coneTouchesObserver(n))
				return nullptr;
		profit -= (I32)w + 1;
		++st.packFrontier;
		return fn.create<PackNode>(vecTy, lanes);
	}

	// isomorphic binary lanes: recurse into the two operand tuples and rebuild the op.
	// hardFail signals "matched but a sub-tuple could not be built" so the caller stops.
	Node* SlpPackPass::Packer::packBinaryLanes(
			const List<Node*>& lanes, Type* elemTy, Type* vecTy, U32 depth, B32& hardFail) {
		U32 w = (U32)lanes.size();
		Opcode op = lanes[0]->getOpcode();
		B32 allBin = true;
		for(Node* n : lanes)
			if(!isa<BinaryNode>(n) || n->getOpcode() != op || n->getType() != elemTy) {
				allBin = false;
				break;
			}
		if(!allBin || !packableBinary(op, elemTy) || (op == Opcode::Mul && !sse41))
			return nullptr;

		List<Node*> ls, rs;
		BinaryNode* b0 = cast<BinaryNode>(lanes[0]);
		ls.push_back(b0->getLHS());
		rs.push_back(b0->getRHS());
		// commutative lanes orient their operands by shape key
		U64 sl0 = shapes(b0->getLHS()), sr0 = shapes(b0->getRHS());
		for(U32 i = 1; i < w; ++i) {
			BinaryNode* b = cast<BinaryNode>(lanes[i]);
			Node* l = b->getLHS();
			Node* r = b->getRHS();
			if(b->isCommutative() && sl0 != sr0 && !shapesDisabled()) {
				U64 sl = shapes(l), sr = shapes(r);
				if(sl != sl0 && sr == sl0 && sl == sr0) {
					std::swap(l, r);
					++st.orientSwaps;
				}
			}
			ls.push_back(l);
			rs.push_back(r);
		}

		Node* lv = packTuple(ls, elemTy, depth + 1);
		Node* rv = packTuple(rs, elemTy, depth + 1);
		if(!lv || !rv) {
			hardFail = true;
			return nullptr;
		}
		profit += (I32)w - 1;
		++interior;
		++st.packBinary;
		return fn.create<BinaryNode>(op, vecTy, lv, rv);
	}

	Node* SlpPackPass::Packer::packLoads(const List<Node*>& lanes,
																			 Type* elemTy,
																			 Type* vecTy,
																			 B32& hardFail) {
		U32 w = (U32)lanes.size();
		U32 esz = elemTy->byteSize(ptrBytes);
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
		if(windowKey && storeWindow && !equal && k0.sameGroup(*windowKey)) {
			I64 sLo = windowKey->constant, sHi = sLo + (I64)(w * esz);
			I64 lLo = k0.constant, lHi = lLo + (I64)(w * esz);
			if(lLo != sLo && lLo < sHi && sLo < lHi) {
				++st.rejectedOverlap;
				hardFail = true;
				return nullptr;
			}
		}

		// all lanes read one pre-window state: a single wide load (or splat)
		if(sharedState && !interWritten->count(first->getMemory()))
			return packWideOrSplat(first->getMemory(), first, k0, elemTy, vecTy, w, equal);

		for(U32 i = 0; i < w; ++i) {
			LoadNode* l = cast<LoadNode>(lanes[i]);
			Node* m = l->getMemory();
			if(m == memIn)
				continue;
			auto it = interWritten->find(m);
			if(it == interWritten->end())
				return nullptr; // some other state, not this window's business
			if(k0.sameGroup(*windowKey)) {
				I64 laneOff = 0;
				if(adjacent)
					laneOff = (I64)(i * esz);
				I64 c = k0.constant + laneOff;
				for(I64 written : it->second)
					if(written == c) {
						hardFail = true; // scalar reads a freshly stored lane
						return nullptr;
					}
			}
		}

		if(!k0.sameGroup(*windowKey)) {
			U32 guardBytes = equal ? esz : w * esz;
			U32 winBytes = storeWindow ? w * windowKey->size : windowKey->size;
			if(!provablyDisjoint(k0, guardBytes, *windowKey, winBytes))
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
			++st.packSplat;
			Node* ld = fn.create<LoadNode>(elemTy, first->getControl(), mem, ptr);
			Node* sp = fn.create<SplatNode>(vecTy, ld);
			splatLoads.push_back({sp, cast<LoadNode>(ld)});
			return sp;
		}
		profit += (I32)w - 1;
		++interior;
		++st.packWideLoad;
		return fn.create<LoadNode>(vecTy, first->getControl(), mem, ptr);
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
			U32 esz = load->getType()->byteSize(ptrBytes);
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
			U32 esz = cs[i].load->getType()->byteSize(ptrBytes);
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
				++st.splatGrouped;
			}
			i += w;
		}
	}
} // namespace rat
