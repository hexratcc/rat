#include "pass/emit/x86/x86_lower.h"

#include "codegen/machine_function.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/opcode.h"
#include "ir/type.h"
#include "target/target.h"
#include "target/x86/x86_asm.h"

namespace rat {
	namespace detail {
		constexpr U8 kIntCc[] = {CC_E, CC_NE, CC_L, CC_LE, CC_B, CC_BE};
		// FEq..FGe -> condition code and operand swap, the swap keeps lt and le NaN-correct
		constexpr U8 kFpCc[] = {CC_E, CC_NE, CC_A, CC_AE, CC_A, CC_AE};
		constexpr U8 kFpSwap[] = {0, 0, 1, 1, 0, 0};
		// FAdd..FDiv -> sse opcode, scalar and packed
		constexpr U8 kSseOp[] = {0x58, 0x5c, 0x59, 0x5e};
		// FEq..FGe -> cmpps/cmppd predicate, gt and ge are lt and le with swapped operands
		constexpr U8 kVecFpPred[] = {0, 4, 1, 2, 1, 2};
		// the prefix of a float op by packed and double, F3 ss, F2 sd, none ps, 66 pd
		constexpr U8 kFpPfx[2][2] = {{0xf3, 0xf2}, {0, 0x66}};
		// Shl, LShr, AShr -> /ext of pslld, psrld, psrad by an imm8
		constexpr U8 kVecShiftExt[] = {6, 2, 4};
		// Shl, LShr, AShr -> pslld, psrld, psrad by an xmm count, +1 for qword, -1 for word lanes
		constexpr U8 kVecShiftOpc[] = {0xf2, 0xd2, 0xe2};
	} // namespace detail

	using detail::kFp;
	using detail::kGp;

	// n has users and all of them are op
	B32 X86LowerPass::onlyUsedBy(const Node* n, Opcode op) {
		for(Node* u : n->getUsers())
			if(u->getOpcode() != op)
				return false;
		return n->hasUsers();
	}

	// sign-extended once at its def, so a 32-bit index pays no movsxd at every use
	B32 X86LowerPass::sextAtDef(const Node* n) {
		const Type* t = n->getType();
		if(!t || !t->isInt() || !fillsOperand(intBits(t)) || isa<LoadNode>(n) || isa<ConstantNode>(n))
			return false;
		for(Node* u : n->getUsers())
			if(u->getOpcode() == Opcode::SExt)
				return true;
		return false;
	}

	B32 X86LowerPass::splatOnlyLoad(const Node* n) {
		const Type* t = n->getType();
		return isa<LoadNode>(n) && t->isInt() && intBits(t) >= 32 && onlyUsedBy(n, Opcode::Splat);
	}

	BinaryNode* X86LowerPass::addSubOf(const ShuffleNode* n) {
		U32 lanes = n->getType()->getVecLanes();
		Node* from[2] = {n->getFirst(), n->getSecond()};
		Node* parity[2] = {nullptr, nullptr}; // source of the even and the odd lanes
		for(U32 i = 0; i < lanes; ++i) {
			U32 s = n->getSource(i);
			Node*& want = parity[i % 2];
			if(s % lanes != i || (want && want != from[s / lanes]))
				return nullptr;
			want = from[s / lanes];
		}
		BinaryNode* sub = dyn_cast<BinaryNode>(parity[0]);
		BinaryNode* add = dyn_cast<BinaryNode>(parity[1]);
		if(!sub || !add || sub->getOpcode() != Opcode::FSub || add->getOpcode() != Opcode::FAdd)
			return nullptr;
		B32 sameOrder = sub->getLHS() == add->getLHS() && sub->getRHS() == add->getRHS();
		// fadd commutes, so gvn may keep it with swapped operands
		B32 swapped = sub->getLHS() == add->getRHS() && sub->getRHS() == add->getLHS();
		if(!sameOrder && !swapped)
			return nullptr;
		return sub;
	}

	B32 X86LowerPass::addSubOnly(const Node* n) {
		Opcode op = n->getOpcode();
		if(!n->getType()->isVec() || (op != Opcode::FAdd && op != Opcode::FSub))
			return false;
		for(Node* u : n->getUsers())
			if(!isa<ShuffleNode>(u) || !addSubOf(cast<ShuffleNode>(u)))
				return false;
		return n->hasUsers();
	}

	U32 X86LowerPass::log2Scale(I64 c) {
		if(c == 2 || c == 4 || c == 8)
			return (U32)countTrailingZeros64((U64)c);
		return 0;
	}

	BinaryNode* X86LowerPass::asAdd(Node* n) {
		BinaryNode* b = dyn_cast<BinaryNode>(n);
		return b && b->getOpcode() == Opcode::Add ? b : nullptr;
	}

	B32 X86LowerPass::scaleOf(Node* n, Node*& idx, U32& scaleLog2) {
		BinaryNode* b = dyn_cast<BinaryNode>(n);
		if(!b || opWidth(n->getType()) != 8)
			return false;
		I64 c = 0;
		Node* x = nullptr;
		U32 sc = 0;
		if(b->getOpcode() == Opcode::Shl && immOf(b->getRHS(), c) && c >= 1 && c <= 3) {
			x = b->getLHS();
			sc = (U32)c;
		} else if(b->getOpcode() == Opcode::Mul) {
			if(immOf(b->getRHS(), c))
				x = b->getLHS();
			else if(immOf(b->getLHS(), c))
				x = b->getRHS();
			sc = log2Scale(c);
		}
		if(!x || sc == 0)
			return false;
		idx = x;
		scaleLog2 = sc;
		return true;
	}

	X86LowerPass::AddrMatch X86LowerPass::decodeAddr(Node* ptr) {
		AddrMatch m;
		Node* work = ptr;
		I64 c;
		if(BinaryNode* add = asAdd(work)) {
			if(immOf(add->getRHS(), c)) {
				m.disp = (I32)c;
				work = add->getLHS();
			} else if(immOf(add->getLHS(), c)) {
				m.disp = (I32)c;
				work = add->getRHS();
			}
		}
		m.base = work;
		BinaryNode* add = asAdd(work);
		if(!add)
			return m;
		Node* ops[2] = {add->getRHS(), add->getLHS()};
		for(U32 i = 0; i < 2; ++i) {
			if(!scaleOf(ops[i], m.index, m.scaleLog2))
				continue;
			m.base = ops[1 - i];
			m.scaleNode = ops[i];
			return m;
		}
		return m;
	}

	X86LowerPass::AddrParts X86LowerPass::matchAddr(Node* ptr) {
		AddrMatch m = decodeAddr(ptr);
		AddrParts a;
		a.disp = m.disp;
		a.scaleLog2 = m.scaleLog2;
		if(AllocNode* al = dyn_cast<AllocNode>(m.base)) {
			a.frameBase = true;
			a.disp += fl->allocOff[al->getId()];
		} else {
			a.base = gpValue(m.base);
		}
		if(m.index)
			a.index = gpValue(m.index);
		return a;
	}

	B32 X86LowerPass::addressOnlyAdd(Node* n) {
		if(!asAdd(n) || n->getUsers().empty())
			return false;
		if(decodeAddr(n).base == n)
			return false; // no disp and no index to fold
		for(Node* u : n->getUsers()) {
			// x87 mem ops take no disp or index, so the add stays
			if(LoadNode* ld = dyn_cast<LoadNode>(u)) {
				if(ld->getPointer() != n || isX87Ty(ld->getType()))
					return false;
			} else if(StoreNode* st = dyn_cast<StoreNode>(u)) {
				if(st->getPointer() != n || st->getValue() == n || isX87Ty(st->getValue()->getType()))
					return false;
			} else if(isa<BinaryNode>(u)) {
				// ptr + const chains decompose n through the user's own addr match
				if(!addressOnlyAdd(u))
					return false;
				AddrMatch um = decodeAddr(u);
				if(um.base == n || um.index == n)
					return false;
			} else {
				return false;
			}
		}
		return true;
	}

	B32 X86LowerPass::addressOnlyScale(Node* n) {
		Node* idx = nullptr;
		U32 sc = 0;
		if(!scaleOf(n, idx, sc) || n->getUsers().empty())
			return false;
		for(Node* u : n->getUsers())
			if(!addressOnlyAdd(u) || decodeAddr(u).scaleNode != n)
				return false;
		return true;
	}

	void X86LowerPass::emitStore(StoreNode* s) {
		Node* val = s->getValue();
		U32 w = opWidth(val->getType());
		if(isX87Ty(val->getType())) {
			VReg addr = gpValue(s->getPointer());
			I32 src = x87Value(val);
			put(X86Op::X87StoreMem, {}, {vr(addr), sl(src)}, detail::kX87MemBits);
			return;
		}
		AddrParts a = matchAddr(s->getPointer());
		if(isSseTy(val->getType()) && !gpSignOp(val))
			return store(X86Op::FStore, a, vr(sseValue(val), w));
		if(ConstantNode* c = dyn_cast<ConstantNode>(val)) {
			I64 v = c->getValue();
			if(w < 8 || v == (I64)(I32)v)
				return store(X86Op::Store, a, im(v, w));
		}
		MachineOperand src = vr(gpValue(val), w);
		// base, index and src may all spill but only 2 gp scratch exist
		if(a.index != kNoVReg && !a.frameBase) {
			VReg t = fresh(kGp);
			lea(t, a);
			return store(X86Op::Store, AddrParts{t}, src);
		}
		store(X86Op::Store, a, src);
	}

	void X86LowerPass::emitLoad(LoadNode* l) {
		U32 w = opWidth(l->getType());
		if(isX87Ty(l->getType())) {
			VReg addr = gpValue(l->getPointer());
			put(X86Op::X87LoadMem, {sl(x87SlotOf(l))}, {vr(addr)}, detail::kX87MemBits);
			return;
		}
		AddrParts a = matchAddr(l->getPointer());
		if(gpSignLoad(l)) {
			vregOf[l->getId()] = fresh(kGp);
			load(X86Op::Load, vregOf[l->getId()], w, a);
		} else if(isSseTy(l->getType())) {
			load(X86Op::FLoad, vregFor(l), w, a);
			if(repeatsLanes(l->getType()) && !readsLowLanes(l))
				repeatLowHalf(vregFor(l));
		} else if(splatOnlyLoad(l)) {
			vregOf[l->getId()] = fresh(kFp); // movss/movsd, no gp round trip
			load(X86Op::FLoad, vregOf[l->getId()], w, a);
		} else {
			// zero-extended unless every user sign-extends, a movsxd load ran slower than a mov on zen 3
			B32 sign = onlyUsedBy(l, Opcode::SExt);
			if(fillsOperand(intBits(l->getType())))
				extOf[l->getId()] = extFor(sign);
			load(X86Op::Load, vregFor(l), w, a, sign);
		}
	}

	void X86LowerPass::maskBits(VReg d, U32 bits) {
		if(bits == 0 || bits >= 64)
			return;
		MachineInstr& m = put(X86Op::MaskBits, {vr(d)}, {vr(d)}, (I64)bits);
		if(bits > 32)
			m.clobbers = detail::physBit(detail::gpPhys(R11)); // a wide mask goes through r11
	}

	// the bits above bits become copies of the sign or zero
	void X86LowerPass::extendBits(VReg d, U32 bits, B32 sign) {
		if(!sign || bits == 1)
			return maskBits(d, bits);
		if(bits > 0 && bits < 64)
			put(X86Op::SignExtBits, {vr(d)}, {vr(d)}, (I64)bits);
	}

	VReg X86LowerPass::widened(VReg s, U32 bits, B32 sign) {
		VReg d = fresh(kGp);
		mov(vr(d), vr(s));
		extendBits(d, bits, sign);
		return d;
	}

	// n for a reader of its whole dword
	VReg X86LowerPass::dwordValue(Node* n, B32 sign) {
		U32 bits = intBits(n->getType());
		if(bits >= 32 || bits == 1 || extOf[n->getId()] == extFor(sign))
			return gpValue(n);
		if(ConstantNode* c = dyn_cast<ConstantNode>(n))
			return gpConst(dwordImm(c->getValue(), bits, sign));
		return widened(gpValue(n), bits, sign);
	}

	// movsx and movzx extend from 8, 16 or 32 bits only
	B32 X86LowerPass::fillsOperand(U32 bits) { return bits == 8 || bits == 16 || bits == 32; }

	B32 X86LowerPass::signedOrder(const CompareNode* n) {
		return n->getOpcode() == Opcode::Slt || n->getOpcode() == Opcode::Sle;
	}

	X86LowerPass::Ext X86LowerPass::extFor(B32 sign) { return sign ? Ext::Sign : Ext::Zero; }

	// v of an int of bits, as a reader of its whole dword sees it
	I64 X86LowerPass::dwordImm(I64 v, U32 bits, B32 sign) {
		if(bits >= 32)
			return v;
		if(sign)
			return signExtend(v, bits);
		return v & (((I64)1 << bits) - 1);
	}

	void X86LowerPass::emitBinary(BinaryNode* n) {
		// clang-format off
		constexpr X86Op kOps[] = {X86Op::Add, X86Op::Sub, X86Op::Mul, X86Op::SDiv, X86Op::UDiv,
			X86Op::SRem, X86Op::URem, X86Op::And, X86Op::Or, X86Op::Xor, X86Op::Shl, X86Op::LShr,
			X86Op::AShr, X86Op::Rotl, X86Op::Rotr};
		// clang-format on
		static_assert((U32)Opcode::Rotr - (U32)Opcode::Add + 1 == 15, "kOps must cover Add..Rotr");
		Opcode op = n->getOpcode();
		if(n->getType() && n->getType()->isVec())
			return emitVecBinary(n);
		if(op >= Opcode::FAdd && op <= Opcode::FDiv)
			return emitFloatBinary(n);
		X86Op mop = kOps[(U32)op - (U32)Opcode::Add];
		if(op >= Opcode::SDiv && op <= Opcode::URem)
			emitDiv(n, mop);
		else if(op >= Opcode::Shl)
			emitShift(n, mop);
		else
			emitAlu(n, mop);
	}

	void X86LowerPass::emitAlu(BinaryNode* n, X86Op op) {
		Node* ln = n->getLHS();
		Node* rn = n->getRHS();
		I64 iv;
		if(op != X86Op::Sub && !immOf(rn, iv) && immOf(ln, iv))
			std::swap(ln, rn); // commutative ops put a lone constant on the RHS
		VReg d = vregFor(n);
		VReg lhs = gpValue(ln);
		// 64-bit forms, a 32-bit add feeding a movsxd index ran 12% slower on zen 3
		if(!immOf(rn, iv)) {
			VReg rhs = gpValue(rn);
			mov(vr(d), vr(lhs));
			alu(op, d, vr(rhs));
		} else if(op != X86Op::Mul) {
			mov(vr(d), vr(lhs));
			alu(op, d, im(iv));
		} else if(U32 sc = log2Scale(iv - 1)) {
			lea(d, AddrParts{lhs, 0, lhs, sc}); // x*3, x*5, x*9
		} else {
			put(X86Op::Mul, {vr(d)}, {vr(lhs), im(iv)}); // three-operand imul, no tied copy
		}
		if(intBits(n->getType()) == 1 && op != X86Op::And && op != X86Op::Or && op != X86Op::Xor)
			maskBits(d, 1); // b-- would wrap a bool to -1
	}

	void X86LowerPass::emitDiv(BinaryNode* n, X86Op op) {
		B32 sign = op == X86Op::SDiv || op == X86Op::SRem;
		VReg lhs = dwordValue(n->getLHS(), sign);
		VReg rhs = dwordValue(n->getRHS(), sign);
		VReg d = vregFor(n);
		mov(ph(R11), vr(rhs));
		mov(ph(RAX), vr(lhs));
		mov(ph(RCX), ph(R11));
		put(op, {ph(RAX), ph(RDX)}, {ph(RAX), ph(RCX)}, (I64)intBits(n->getType()));
		B32 wantRem = op == X86Op::SRem || op == X86Op::URem;
		mov(vr(d), ph(wantRem ? RDX : RAX));
	}

	// shifts and rotates, rotates come only with a constant count from the fold rule
	void X86LowerPass::emitShift(BinaryNode* n, X86Op op) {
		B32 rotate = op == X86Op::Rotl || op == X86Op::Rotr;
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		U32 bits = intBits(n->getType());
		U32 w = gpWidth(n->getType());
		mov(vr(d), vr(lhs));
		// a narrow right shift pulls in the bits above the width
		if(bits < 32 && (op == X86Op::LShr || op == X86Op::AShr))
			extendBits(d, bits, op == X86Op::AShr);
		I64 count = 0;
		B32 immCount = immOf(n->getRHS(), count);
		if(rotate && !immCount)
			return; // degenerate, should not happen
		if(rotate) {
			put(op, {vr(d)}, {vr(d), im(count & (bits - 1))}, (I64)bits);
		} else if(immCount) {
			alu(op, d, im(count & (w * 8 - 1)), w);
		} else {
			VReg rhs = gpValue(n->getRHS());
			mov(ph(RCX), vr(rhs));
			alu(op, d, ph(RCX), w);
		}
	}

	void X86LowerPass::emitFloatBinary(BinaryNode* n) {
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::FAdd;
		if(isX87Ty(n->getType())) {
			constexpr X86Op kX87Arith[] = {X86Op::X87Add, X86Op::X87Sub, X86Op::X87Mul, X86Op::X87Div};
			I32 lhs = x87Value(n->getLHS());
			I32 rhs = x87Value(n->getRHS());
			put(kX87Arith[idx], {sl(x87SlotOf(n))}, {sl(lhs), sl(rhs)});
			return;
		}
		U32 w = opWidth(n->getType());
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		farith(scalarEnc(detail::kSseOp[idx], w), d, lhs, rhs, w);
	}

	void X86LowerPass::emitVecBinary(BinaryNode* n) {
		// Add..Xor -> the packed int opcode per log2 of the lane bytes, 0 none, pmulld is 0f38
		// padd psub pmull(w d) pand por pxor, byte lanes multiply in emitByteMul
		constexpr U8 kIntOps[4][10] = {{0xfc, 0xf8, 0, 0, 0, 0, 0, 0xdb, 0xeb, 0xef},
																	 {0xfd, 0xf9, 0xd5, 0, 0, 0, 0, 0xdb, 0xeb, 0xef},
																	 {0xfe, 0xfa, 0x40, 0, 0, 0, 0, 0xdb, 0xeb, 0xef},
																	 {0xd4, 0xfb, 0, 0, 0, 0, 0, 0xdb, 0xeb, 0xef}};
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		Opcode op = n->getOpcode();
		if(isShiftOpcode(op))
			return emitVecShift(n);
		if(et->isInt() && op == Opcode::Mul && esz == 1)
			return emitByteMul(n);
		Enc e{0x66, 0x0f, 0};
		if(et->isInt() && op <= Opcode::Xor) {
			e.opc = kIntOps[countTrailingZeros64(esz)][(U32)op - (U32)Opcode::Add];
			if(op == Opcode::Mul && esz == 4)
				e.map = 0x38;
		} else if(!et->isInt() && op >= Opcode::FAdd) {
			e.pfx = esz == 8 ? 0x66 : 0; // addpd... vs addps...
			e.opc = detail::kSseOp[(U32)op - (U32)Opcode::FAdd];
		}
		if(e.opc == 0)
			return;
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		varith(e, d, lhs, rhs);
	}

	void X86LowerPass::emitVecShift(BinaryNode* n) {
		U32 bits = opWidth(n->getType()->getVecElement()) * 8;
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::Shl;
		I64 cnt = 0;
		B32 constant = immOf(n->getRHS(), cnt);
		if(bits == 8) // a constant amount, see verify
			return emitByteShift(n, cnt);
		VReg s = sseValue(n->getLHS());
		VReg d = vregFor(n);
		if(constant) {
			mov(vr(d, 16), vr(s, 16));
			vshiftImm(d, bits, idx, cnt);
			return;
		}
		U8 opc = detail::kVecShiftOpc[idx];
		if(bits == 64)
			++opc;
		if(bits == 16)
			--opc;
		VReg c = fresh(kFp);
		VReg countReg = gpValue(n->getRHS());
		sse(enc::kMovdToXmm, vr(c, 16), {vr(countReg)});
		varith({0x66, 0x0f, opc}, d, s, c);
	}

	// d shifted in place, idx 0 1 2 for shl lshr ashr
	void X86LowerPass::vshiftImm(VReg d, U32 laneBits, U32 idx, I64 cnt) {
		I64 extAndCnt = ((I64)detail::kVecShiftExt[idx] << 8) | (U8)cnt;
		put(X86Op::VShiftImm, {vr(d, 16)}, {vr(d, 16)}, (I64)laneBits, extAndCnt);
	}

	void X86LowerPass::emitByteShift(BinaryNode* n, I64 cnt) {
		Opcode op = n->getOpcode();
		B32 left = op == Opcode::Shl;
		U8 kept = left ? (U8)(0xff << cnt) : (U8)(0xff >> cnt);
		VReg d = vregFor(n);
		VReg mask = fresh(kFp);
		mov(vr(d, 16), vr(sseValue(n->getLHS()), 16));
		vshiftImm(d, 16, left ? 0 : 1, cnt); // psllw, psrlw
		loadVec(mask, List<U8>(16, kept));
		varith({0x66, 0x0f, 0xdb}, d, d, mask); // pand
		if(op != Opcode::AShr)
			return;
		VReg sign = fresh(kFp);
		loadVec(sign, List<U8>(16, (U8)(0x80 >> cnt)));
		varith({0x66, 0x0f, 0xef}, d, d, sign); // pxor
		varith({0x66, 0x0f, 0xf8}, d, d, sign); // psubb
	}

	void X86LowerPass::emitByteMul(BinaryNode* n) {
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		VReg odd = fresh(kFp);
		VReg rhsOdd = fresh(kFp);
		varith({0x66, 0x0f, 0xd5}, d, lhs, rhs); // pmullw, the even bytes
		vshiftImm(d, 16, 0, 8);
		vshiftImm(d, 16, 1, 8); // clears the odd bytes
		mov(vr(odd, 16), vr(lhs, 16));
		vshiftImm(odd, 16, 1, 8);
		mov(vr(rhsOdd, 16), vr(rhs, 16));
		vshiftImm(rhsOdd, 16, 1, 8);
		varith({0x66, 0x0f, 0xd5}, odd, odd, rhsOdd); // the odd bytes
		vshiftImm(odd, 16, 0, 8);
		varith({0x66, 0x0f, 0xeb}, d, d, odd); // por
	}

	void X86LowerPass::emitSplat(SplatNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		B32 inGp = !et->isFloat() && !splatOnlyLoad(n->getScalar());
		VReg s = classValue(n->getScalar(), inGp ? kGp : kFp);
		U32 sw = inGp ? 8 : esz;
		put(X86Op::VSplat, {vr(vregFor(n), 16)}, {vr(s, sw)}, (I64)esz, inGp);
	}

	void X86LowerPass::emitExtract(ExtractNode* n) {
		Type* et = n->getType();
		U32 esz = et->byteSize(ptrBytes);
		B32 isInt = !et->isFloat();
		VReg v = sseValue(n->getVector());
		VReg d = vregFor(n);
		// float lane 0 is the low bits of the vector, a copy the allocator can coalesce
		if(!isInt && n->getLane() == 0)
			return mov(vr(d, esz), vr(v, 16));
		if(isInt && n->getLane() != 0 && fl->vecScratch == 0)
			fl->vecScratch = reserve(16); // staged through memory
		I64 desc = ((I64)esz << 1) | isInt;
		U32 dw = isInt ? 8 : esz;
		put(X86Op::VExtract, {vr(d, dw)}, {vr(v, 16)}, n->getLane(), desc);
	}

	// the constant lanes of n as 16 pool bytes, zero in the other lanes
	List<U8> X86LowerPass::constLanes(PackNode* n, U32 esz) {
		List<U8> bytes(16, 0);
		for(U32 i = 0; i < n->getLaneCount(); ++i) {
			ConstantNode* c = dyn_cast<ConstantNode>(n->getLane(i));
			if(!c)
				continue;
			U64 v = (U64)c->getValue();
			for(U32 b = 0; b < esz; ++b)
				bytes[i * esz + b] = (U8)(v >> (8 * b));
			if(repeatsLanes(n->getType()))
				for(U32 b = 0; b < esz; ++b)
					bytes[(i + 2) * esz + b] = (U8)(v >> (8 * b));
		}
		return bytes;
	}

	// the constant lanes come from the pool, the others are inserted one by one
	void X86LowerPass::emitPack(PackNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		B32 isInt = !et->isFloat();
		List<U8> bytes = constLanes(n, esz);
		B32 nonZero = std::count(bytes.begin(), bytes.end(), 0) < 16;
		B32 fromPool = nonZero || isa<ConstantNode>(n->getLane(0));
		VReg d = vregFor(n);
		if(fromPool)
			loadVec(d, bytes);
		else
			packLaneZero(n, d, esz, isInt);
		B32 allConstant = true;
		for(U32 i = 0; i < n->getLaneCount(); ++i) {
			if(isa<ConstantNode>(n->getLane(i)))
				continue;
			allConstant = false;
			if(i || fromPool)
				insertLane(d, n->getLane(i), i, esz, isInt);
		}
		// the pool bytes repeat the constant lanes already, see constLanes
		if(repeatsLanes(n->getType()) && !allConstant && !readsLowLanes(n))
			repeatLowHalf(d);
	}

	void X86LowerPass::packLaneZero(PackNode* n, VReg d, U32 esz, B32 isInt) {
		Node* lane = n->getLane(0);
		if(isInt) {
			VReg s = gpValue(lane);
			Enc gpToXmm = esz == 8 ? enc::kMovqToXmm : enc::kMovdToXmm;
			sse(gpToXmm, vr(d, 16), {vr(s)}); // the upper lanes zero
			return;
		}
		U8 zero = 0; // insertps zero mask
		for(U32 i = 1; i < n->getLaneCount(); ++i)
			if(isa<ConstantNode>(n->getLane(i)))
				zero |= (U8)(1 << i);
		VReg s = sseValue(lane);
		if(zero && esz == 8) {
			sse({0xf3, 0x0f, 0x7e}, vr(d, 16), {vr(s, 16)}); // movq
			return;
		}
		mov(vr(d, esz), vr(s, esz));
		if(zero)
			vmerge({0x66, 0x3a, 0x21}, d, d, zero); // insertps, zeroing the constant lanes
	}

	void X86LowerPass::insertLane(VReg d, Node* lane, U32 i, U32 esz, B32 isInt) {
		if(isInt) {
			VReg s = gpValue(lane);
			sse({0x66, 0x3a, 0x22, esz == 8}, vr(d, 16), {vr(d, 16), vr(s)}, (I32)i); // pinsrd, pinsrq
			return;
		}
		VReg s = sseValue(lane);
		if(esz == 4)
			vmerge({0x66, 0x3a, 0x21}, d, s, (I32)(i << 4)); // insertps
		else if(i == 0)
			vmerge({0xf2, 0x0f, 0x10}, d, s); // movsd
		else
			vmerge({0, 0x0f, 0x16}, d, s); // movlhps
	}

	// lanes 2 and 3 of d copy lanes 0 and 1, see repeatsLanes
	void X86LowerPass::repeatLowHalf(VReg d) { vmerge({0, 0x0f, 0x16}, d, d); } // movlhps

	void X86LowerPass::shuffleDwords(const ShuffleNode* n, U8& sel, U8& mask, B32& inPlace) {
		Type* vt = n->getType();
		U32 lanes = vt->getVecLanes();
		U32 per = opWidth(vt->getVecElement()) / 4;
		U32 src[4] = {0, 1, 2, 3};
		for(U32 i = 0; i < lanes; ++i) {
			U32 s = n->getSource(i);
			for(U32 k = 0; k < per; ++k)
				src[i * per + k] = s / lanes * 4 + s % lanes * per + k;
		}
		if(repeatsLanes(vt)) {
			src[2] = src[0];
			src[3] = src[1];
		}
		sel = 0;
		mask = 0;
		inPlace = true;
		for(U32 i = 0; i < 4; ++i) {
			if(n->getFirst() == n->getSecond())
				src[i] &= 3;
			sel |= (U8)((src[i] & 3) << (2 * i));
			mask |= (U8)((src[i] >> 2) << i);
			inPlace &= (src[i] & 3) == i;
		}
	}

	void X86LowerPass::emitShuffle(ShuffleNode* n) {
		if(BinaryNode* sub = addSubOf(n)) {
			U8 pfx = opWidth(n->getType()->getVecElement()) == 8 ? 0x66 : 0xf2; // addsubpd, addsubps
			VReg rhs = sseValue(sub->getRHS());
			VReg lhs = sseValue(sub->getLHS());
			varith({pfx, 0x0f, 0xd0}, vregFor(n), lhs, rhs);
			return;
		}
		U8 sel = 0, mask = 0;
		B32 inPlace = true;
		shuffleDwords(n, sel, mask, inPlace);
		VReg a = sseValue(n->getFirst());
		VReg b = sseValue(n->getSecond());
		VReg d = vregFor(n);
		if(inPlace && mask != 0 && mask != 15) {
			varith({0x66, 0x3a, 0x0c}, d, a, b, mask); // blendps
			return;
		}
		if(mask == 15 || mask == 3)
			std::swap(a, b); // the low half comes from second
		if(mask == 0 || mask == 15) {
			sse(enc::kPshufd, vr(d, 16), {vr(a, 16)}, sel);
		} else if(mask == 12 || mask == 3) {
			varith({0, 0x0f, 0xc6}, d, a, b, sel); // shufps
		} else {
			VReg ta = fresh(kFp);
			VReg tb = fresh(kFp);
			sse(enc::kPshufd, vr(ta, 16), {vr(a, 16)}, sel);
			sse(enc::kPshufd, vr(tb, 16), {vr(b, 16)}, sel);
			varith({0x66, 0x3a, 0x0c}, d, ta, tb, mask);
		}
	}

	void X86LowerPass::emitBitScan(UnaryNode* n, B32 reverse) {
		U32 bits = intBits(n->getType());
		U32 w = gpWidth(n->getType()) * 8;
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		if(bits < w) {
			mov(vr(d), vr(s));
			maskBits(d, bits);
			s = d;
		}
		X86Op scan = reverse ? X86Op::BitScanR : X86Op::BitScanF;
		put(scan, {vr(d)}, {vr(s)}, (I64)w);
		if(reverse)
			alu(X86Op::Xor, d, im((I64)bits - 1));
	}

	void X86LowerPass::emitPopcnt(UnaryNode* n) {
		constexpr I64 k55 = (I64)0x5555555555555555ull;
		constexpr I64 k33 = (I64)0x3333333333333333ull;
		constexpr I64 k0f = (I64)0x0f0f0f0f0f0f0f0full;
		constexpr I64 k01 = (I64)0x0101010101010101ull;
		U32 bits = intBits(n->getType());
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		VReg t = fresh(kGp);
		mov(vr(d), vr(s));
		maskBits(d, bits);
		// d -= (d >> 1) & k55
		mov(vr(t), vr(d));
		alu(X86Op::LShr, t, im(1));
		alu(X86Op::And, t, vr(gpConst(k55)));
		alu(X86Op::Sub, d, vr(t));
		// d = (d & k33) + ((d >> 2) & k33)
		VReg m33 = gpConst(k33);
		mov(vr(t), vr(d));
		alu(X86Op::LShr, t, im(2));
		alu(X86Op::And, t, vr(m33));
		alu(X86Op::And, d, vr(m33));
		alu(X86Op::Add, d, vr(t));
		// d = (d + (d >> 4)) & k0f
		mov(vr(t), vr(d));
		alu(X86Op::LShr, t, im(4));
		alu(X86Op::Add, d, vr(t));
		alu(X86Op::And, d, vr(gpConst(k0f)));
		// every byte now holds its own count
		alu(X86Op::Mul, d, vr(gpConst(k01)));
		alu(X86Op::LShr, d, im(56));
	}

	void X86LowerPass::emitFNeg(UnaryNode* n) {
		if(gpSignOp(n))
			return emitGpSign(n);
		if(isX87Ty(n->getType())) {
			I32 s = x87Value(n->getOperand());
			put(X86Op::X87Neg, {sl(x87SlotOf(n))}, {sl(s)});
			return;
		}
		U32 w = regWidth(n->getType());
		U32 lane = n->getType()->isVec() ? opWidth(n->getType()->getVecElement()) : w;
		VReg s = sseValue(n->getOperand());
		needScratch();
		// the encoder builds the sign mask of each lane through the top volatile xmm
		MachineInstr& m = put(X86Op::FNeg, {vr(vregFor(n), w)}, {vr(s, w)}, (I64)lane);
		m.clobbers = detail::physBit(detail::xmmPhys(conv->sseVolatileCount - 1));
	}

	// the sign bit cleared or flipped in a gp copy of the loaded bits, see gpSignOp
	void X86LowerPass::emitGpSign(UnaryNode* n) {
		U32 w = opWidth(n->getType());
		U64 sign = 1ull << (8 * w - 1);
		B32 abs = n->getOpcode() == Opcode::FAbs;
		VReg d = fresh(kGp);
		vregOf[n->getId()] = d;
		mov(vr(d), vr(vregFor(n->getOperand())));
		X86Op op = abs ? X86Op::And : X86Op::Xor;
		I64 mask = (I64)(abs ? sign - 1 : sign);
		if(w == 4)
			alu(op, d, im(mask)); // as imm32, the store keeps the low 32 bits
		else
			alu(op, d, vr(gpConst(mask)));
	}

	// sqrtps or sqrtpd for a vector, fabs shifts the sign bit out of each lane
	void X86LowerPass::emitFSqrtAbs(UnaryNode* n) {
		if(gpSignOp(n))
			return emitGpSign(n);
		Type* type = n->getType();
		U32 w = regWidth(type);
		U32 lane = type->isVec() ? opWidth(type->getVecElement()) : w;
		VReg s = sseValue(n->getOperand());
		VReg d = vregFor(n);
		if(n->getOpcode() == Opcode::FAbs)
			put(X86Op::FAbs, {vr(d, w)}, {vr(s, w)}, (I64)lane);
		else
			sse({detail::kFpPfx[type->isVec()][lane == 8], 0x0f, 0x51}, vr(d, w), {vr(s, w)});
	}

	void X86LowerPass::emitUnary(UnaryNode* n) {
		Opcode uop = n->getOpcode();
		if(uop == Opcode::Clz || uop == Opcode::Ctz)
			return emitBitScan(n, uop == Opcode::Clz);
		if(uop == Opcode::Popcnt)
			return emitPopcnt(n);
		if(uop == Opcode::FNeg)
			return emitFNeg(n);
		if(uop == Opcode::FSqrt || uop == Opcode::FAbs)
			return emitFSqrtAbs(n);
		U32 bits = intBits(n->getType());
		U32 w = gpWidth(n->getType());
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		mov(vr(d), vr(s));
		if(uop == Opcode::Bswap) {
			put(X86Op::Bswap, {vr(d)}, {vr(d)}, (I64)w * 8);
		} else if(uop == Opcode::Neg) {
			put(X86Op::Neg, {vr(d, w)}, {vr(d, w)});
			if(bits == 1)
				maskBits(d, 1);
		} else {
			put(X86Op::Not, {vr(d)}, {vr(d)});
		}
	}

	// a constant holds its int sign-extended, see gpValue, and zero-extended too when not negative
	B32 X86LowerPass::holdsExt(const Node* n, Ext ext) const {
		const ConstantNode* c = dyn_cast<ConstantNode>(n);
		if(!c)
			return extOf[n->getId()] == ext;
		return ext == Ext::Sign || signExtend(c->getValue(), intBits(n->getType())) >= 0;
	}

	// sign extension keeps the signed and the unsigned order, zero extension the unsigned one
	B32 X86LowerPass::fitsWideCmp(const CompareNode* n) const {
		if(!fillsOperand(intBits(n->getLHS()->getType())))
			return false;
		if(holdsExt(n->getLHS(), Ext::Sign) && holdsExt(n->getRHS(), Ext::Sign))
			return true;
		B32 zero = holdsExt(n->getLHS(), Ext::Zero) && holdsExt(n->getRHS(), Ext::Zero);
		return zero && !signedOrder(n);
	}

	// both sides extended alike, see fitsWideCmp, so one 64-bit compare
	void X86LowerPass::emitWideCmp(CompareNode* n) {
		VReg lhs = gpValue(n->getLHS());
		I64 iv;
		if(immOf(n->getRHS(), iv))
			cmp(lhs, im(signExtend(iv, intBits(n->getLHS()->getType()))));
		else
			cmp(lhs, vr(gpValue(n->getRHS())));
	}

	// each side at its own width, a narrow one widened to a dword
	void X86LowerPass::emitNarrowCmp(CompareNode* n) {
		const Type* t = n->getLHS()->getType();
		B32 sign = signedOrder(n);
		VReg lhs = dwordValue(n->getLHS(), sign);
		I64 iv;
		if(immOf(n->getRHS(), iv))
			cmp(lhs, im(dwordImm(iv, intBits(t), sign)), gpWidth(t));
		else
			cmp(lhs, vr(dwordValue(n->getRHS(), sign)), gpWidth(t));
	}

	// ucomis lhs, rhs, lt and le swap the operands, see kFpSwap
	U8 X86LowerPass::emitFpCmp(CompareNode* n) {
		U32 w = opWidth(n->getLHS()->getType());
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::FEq;
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		put(X86Op::FCmpFlags, {}, {vr(lhs, w), vr(rhs, w)}, 0, detail::kFpSwap[idx]);
		return detail::kFpCc[idx];
	}

	// flags only, returns the cc for the caller's jcc, setcc or cmov
	U8 X86LowerPass::emitCmp(CompareNode* n) {
		if(n->getOpcode() >= Opcode::FEq)
			return emitFpCmp(n);
		if(fitsWideCmp(n))
			emitWideCmp(n);
		else
			emitNarrowCmp(n);
		return detail::kIntCc[(U32)n->getOpcode() - (U32)Opcode::Eq];
	}

	void X86LowerPass::emitSelect(SelectNode* n) {
		if(n->getType()->isVec())
			return emitVecSelect(n);
		Node* cond = n->getCondition();
		// a scalar float select is a min or max, see checkSelect in verify.cpp
		if(n->getType()->isFloat()) {
			emitMinMax(n, cast<CompareNode>(cond));
			return;
		}
		VReg d = vregFor(n);
		// materialize both arms before the compare
		VReg f = gpValue(n->getFalse());
		VReg t = gpValue(n->getTrue());
		mov(vr(d), vr(f));
		U8 cc = CC_NE;
		if(selectOnlyCompare(cond)) {
			cc = emitCmp(cast<CompareNode>(cond));
		} else {
			VReg cv = gpValue(cond);
			cmp(cv, im(0));
		}
		put(X86Op::CMov, {vr(d)}, {vr(d), vr(t)}, (I64)cc);
	}

	VReg X86LowerPass::vecMask(CompareNode* c, B32& invert) {
		Type* et = c->getLHS()->getType()->getVecElement();
		U32 esz = opWidth(et);
		Opcode op = c->getOpcode();
		VReg l = sseValue(c->getLHS());
		VReg r = sseValue(c->getRHS());
		invert = op == Opcode::Ne || op == Opcode::Sle || op == Opcode::Ult;
		if(!et->isFloat())
			return vecIntMask(op, l, r, esz);
		VReg m = fresh(kFp);
		if(op == Opcode::FGt || op == Opcode::FGe)
			std::swap(l, r);
		U8 pfx = esz == 8 ? 0x66 : 0;
		U8 pred = detail::kVecFpPred[(U32)op - (U32)Opcode::FEq];
		varith({pfx, 0x0f, 0xc2}, m, l, r, pred); // cmpps, cmppd
		return m;
	}

	VReg X86LowerPass::vecIntMask(Opcode op, VReg l, VReg r, U32 esz) {
		VReg m = fresh(kFp);
		if(op == Opcode::Eq || op == Opcode::Ne) {
			if(esz == 8)
				varith({0x66, 0x38, 0x29}, m, l, r); // pcmpeqq
			else
				varith({0x66, 0x0f, 0x76}, m, l, r); // pcmpeqd
			return m;
		}
		if(op == Opcode::Slt || op == Opcode::Sle) {
			if(op == Opcode::Slt)
				std::swap(l, r);
			varith({0x66, 0x0f, 0x66}, m, l, r); // pcmpgtd
			return m;
		}
		if(op == Opcode::Ult)
			std::swap(l, r);
		VReg t = fresh(kFp);
		varith({0x66, 0x38, 0x3b}, t, l, r); // pminud
		varith({0x66, 0x0f, 0x76}, m, t, l); // pcmpeqd
		return m;
	}

	// a select of its compare's operands as one min or max op, scalar float or vector
	B32 X86LowerPass::emitMinMax(SelectNode* n, CompareNode* c) {
		Opcode op = c->getOpcode();
		B32 isSigned = signedOrder(c);
		B32 isUnsigned = op == Opcode::Ult || op == Opcode::Ule;
		if(!armsAreOperands(c, n->getTrue(), n->getFalse()))
			return false;
		// picking the rhs of a less-than is a max, of a greater-than a min
		B32 picksRhs = !picksCompareLhs(n);
		if(isFloatOrderOpcode(op)) {
			emitFMinMax(n, picksRhs == (op == Opcode::FLt));
			return true;
		}
		if(!(isSigned || isUnsigned) || opWidth(n->getType()->getVecElement()) != 4)
			return false;
		U8 opc = 0x39; // pminsd, pminud 0x3b, pmaxsd 0x3d, pmaxud 0x3f
		if(isUnsigned)
			opc += 2;
		if(picksRhs)
			opc += 4;
		VReg rhs = sseValue(c->getRHS());
		VReg lhs = sseValue(c->getLHS());
		varith({0x66, 0x38, opc}, vregFor(n), lhs, rhs);
		return true;
	}

	// minps t, f picks t when t < f, else f, also for NaN and signed zero. maxps likewise for t > f
	void X86LowerPass::emitFMinMax(SelectNode* n, B32 max) {
		Type* type = n->getType();
		Type* et = type->isVec() ? type->getVecElement() : type;
		Enc e{detail::kFpPfx[type->isVec()][opWidth(et) == 8], 0x0f, max ? (U8)0x5f : (U8)0x5d};
		VReg t = sseValue(n->getTrue());
		VReg f = sseValue(n->getFalse());
		farith(e, vregFor(n), t, f, regWidth(type));
	}

	void X86LowerPass::emitVecSelect(SelectNode* n) {
		CompareNode* c = cast<CompareNode>(n->getCondition());
		if(emitMinMax(n, c))
			return;
		B32 invert = false;
		VReg m = vecMask(c, invert);
		// an int compare of <2 x i32> leaves lanes 2 and 3 of its mask open, a float compare repeats them
		B32 intMask = !c->getLHS()->getType()->getVecElement()->isFloat();
		if(repeatsLanes(n->getType()) && intMask) {
			VReg repeated = fresh(kFp);
			sse(enc::kPshufd, vr(repeated, 16), {vr(m, 16)}, 0x44);
			m = repeated;
		}
		Node* t = n->getTrue();
		Node* f = n->getFalse();
		if(invert)
			std::swap(t, f);
		U8 opc = opWidth(n->getType()->getVecElement()) == 8 ? 0x15 : 0x14; // blendvpd, blendvps
		VReg onTrue = sseValue(t);
		VReg onFalse = sseValue(f);
		VReg d = vregFor(n);
		// d takes onTrue in the lanes m sets, blendv reads the mask from xmm0
		mov(xm(0, 16), vr(m, 16));
		varith({0x66, 0x38, opc}, d, onFalse, onTrue).uses.push_back(xm(0, 16));
	}

	void X86LowerPass::emitCompare(CompareNode* n) {
		Opcode op = n->getOpcode();
		if(op < Opcode::FEq) {
			U8 cc = emitCmp(n);
			put(X86Op::SetCC, {vr(vregFor(n))}, {}, (I64)cc);
			return;
		}
		U32 idx = (U32)op - (U32)Opcode::FEq;
		U8 cc = detail::kFpCc[idx];
		B32 swap = detail::kFpSwap[idx] != 0;
		VReg d = vregFor(n);
		if(isX87Ty(n->getLHS()->getType())) {
			I32 lhs = x87Value(n->getLHS());
			I32 rhs = x87Value(n->getRHS());
			put(X86Op::X87Cmp, {vr(d)}, {sl(lhs), sl(rhs)}, (I64)cc, swap);
		} else {
			U32 w = opWidth(n->getLHS()->getType());
			VReg lhs = sseValue(n->getLHS());
			VReg rhs = sseValue(n->getRHS());
			put(X86Op::FCmp, {vr(d)}, {vr(lhs, w), vr(rhs, w)}, (I64)cc, swap);
		}
		if(op != Opcode::FEq && op != Opcode::FNe)
			return;
		U8 parityCc = op == Opcode::FEq ? CC_NP : CC_P;
		X86Op combine = op == Opcode::FEq ? X86Op::And : X86Op::Or;
		VReg t = fresh(kGp);
		put(X86Op::SetCC, {vr(t)}, {}, parityCc);
		alu(combine, d, vr(t));
	}

	void X86LowerPass::emitConvert(ConvertNode* n) {
		Node* src = n->getOperand();
		if(n->getType()->isVec())
			return emitVecConvert(n);
		if(isX87Ty(n->getType()) || isX87Ty(src->getType()))
			return emitConvertX87(n);
		switch(n->getOpcode()) {
		case Opcode::Trunc:
		case Opcode::SExt:
		case Opcode::ZExt:
			return emitIntResize(n);
		case Opcode::SIToFP:
		case Opcode::UIToFP:
			return emitIntToFP(n);
		case Opcode::FPToSI:
		case Opcode::FPToUI:
			return emitFPToInt(n);
		case Opcode::FPExt:			// f32 -> f64 by cvtss2sd
		case Opcode::FPTrunc: { // f64 -> f32 by cvtsd2ss
			U32 sw = opWidth(src->getType());
			VReg s = sseValue(src);
			sse(scalarEnc(0x5a, sw), vr(vregFor(n), opWidth(n->getType())), {vr(s, sw)});
			return;
		}
		default:
			return;
		}
	}

	void X86LowerPass::emitVecConvert(ConvertNode* n) {
		if(n->getOpcode() == Opcode::Trunc)
			return emitVecTrunc(n);
		Node* src = n->getOperand();
		U32 sw = opWidth(src->getType()->getVecElement());
		U32 dw = opWidth(n->getType()->getVecElement());
		Opcode op = n->getOpcode();
		Enc e{0, 0x0f, 0x5a}; // cvtps2pd
		if(op == Opcode::SIToFP && dw == 8) {
			e = {0xf3, 0x0f, 0xe6}; // cvtdq2pd
		} else if(op == Opcode::SIToFP) {
			e = {0, 0x0f, 0x5b}; // cvtdq2ps
		} else if(op == Opcode::FPToSI && sw == 8) {
			e = {0x66, 0x0f, 0xe6}; // cvttpd2dq
		} else if(op == Opcode::FPToSI) {
			e = {0xf3, 0x0f, 0x5b}; // cvttps2dq
		} else if(op == Opcode::FPTrunc) {
			e = {0x66, 0x0f, 0x5a}; // cvtpd2ps
		} else if(op == Opcode::SExt || op == Opcode::ZExt) {
			e = {0x66, 0x38, 0x23}; // pmovsxwd
			if(sw == 1)
				e.opc = 0x21; // pmovsxbd
			else if(sw == 4)
				e.opc = 0x25; // pmovsxdq
			else if(dw == 8)
				e.opc = 0x24; // pmovsxwq
			if(op == Opcode::ZExt)
				e.opc += 0x10; // pmovzx
		}
		VReg s = sseValue(src);
		if(op == Opcode::SIToFP && repeatsLanes(n->getType())) {
			VReg repeated = fresh(kFp);
			sse(enc::kPshufd, vr(repeated, 16), {vr(s, 16)}, 0x44); // the int lanes 0 1 0 1
			s = repeated;
		}
		sse(e, vr(vregFor(n), 16), {vr(s, 16)});
		if(op == Opcode::FPTrunc && repeatsLanes(n->getType()) && !readsLowLanes(n))
			repeatLowHalf(vregFor(n)); // cvtpd2ps zeroes lanes 2 and 3
	}

	// pshufb picks the low bytes of each lane
	void X86LowerPass::emitVecTrunc(ConvertNode* n) {
		Node* src = n->getOperand();
		U32 sw = opWidth(src->getType()->getVecElement());
		U32 dw = opWidth(n->getType()->getVecElement());
		List<U8> picks(16, 0x80); // bit 7 zeroes a byte
		for(U32 i = 0; i < n->getType()->getVecLanes() * dw; ++i)
			picks[i] = (U8)(i / dw * sw + i % dw);
		VReg mask = fresh(kFp);
		loadVec(mask, picks);
		varith({0x66, 0x38, 0x00}, vregFor(n), sseValue(src), mask);
	}

	void X86LowerPass::emitIntResize(ConvertNode* n) {
		Node* src = n->getOperand();
		Opcode op = n->getOpcode();
		VReg s = gpValue(src);
		VReg d = vregFor(n);
		mov(vr(d), vr(s));
		U32 srcBits = intBits(src->getType());
		if(op == Opcode::Trunc && intBits(n->getType()) == 1)
			maskBits(d, 1);
		if(op == Opcode::Trunc || srcBits == 1)
			return;
		B32 sign = op == Opcode::SExt;
		if(extOf[src->getId()] == extFor(sign))
			return; // extended at its def, see emitLoad, extendAtDef and emitPhiCopies
		extendBits(d, srcBits, sign);
	}

	void X86LowerPass::emitIntToFP(ConvertNode* n) {
		Node* src = n->getOperand();
		B32 isUnsigned = n->getOpcode() == Opcode::UIToFP;
		U32 w = opWidth(n->getType());
		U32 sb = intBits(src->getType());
		VReg s = gpValue(src);
		if(isUnsigned && sb >= 64)
			return emitU64ToFP(n, s, w);
		// an unsigned int converts from its zero-extended qword
		B32 wide = isUnsigned || sb > 32;
		if(isUnsigned || (sb > 1 && sb < 32))
			s = widened(s, sb, !isUnsigned);
		U32 sw = wide ? 8 : 4;
		sse(scalarEnc(0x2a, w, wide), vr(vregFor(n), w), {vr(s, sw)});
	}

	void X86LowerPass::emitFPToInt(ConvertNode* n) {
		Node* src = n->getOperand();
		B32 isUnsigned = n->getOpcode() == Opcode::FPToUI;
		U32 bits = intBits(n->getType());
		U32 w = opWidth(src->getType());
		VReg s = sseValue(src);
		if(isUnsigned && bits >= 64) {
			VReg k = fresh(kFp);
			loadFp(k, w == 4 ? 0x5f000000ull : 0x43e0000000000000ull, w); // 2^63
			return emitFPToU64(n, vr(s, w), vr(k, w));
		}
		B32 wide = isUnsigned || bits > 32;
		sse(scalarEnc(0x2c, w, wide), vr(vregFor(n)), {vr(s, w)});
	}

	Pair<VReg, VReg> X86LowerPass::splitHalves(VReg s) {
		VReg hi = fresh(kGp);
		mov(vr(hi), vr(s));
		alu(X86Op::LShr, hi, im(32));
		VReg lo = widened(s, 32, false);
		return {hi, lo};
	}

	void X86LowerPass::emitU64ToFP(ConvertNode* n, VReg s, U32 w) {
		auto [hi, lo] = splitHalves(s);
		Enc toDouble = scalarEnc(0x2a, 8, true); // cvtsi2sd
		VReg dh = fresh(kFp);
		sse(toDouble, vr(dh), {vr(hi)});
		VReg dl = fresh(kFp);
		sse(toDouble, vr(dl), {vr(lo)});
		VReg scaled = fresh(kFp);
		VReg k = fresh(kFp);
		loadFp(k, 0x41f0000000000000ull, 8);
		farith(scalarEnc(0x59, 8), scaled, dh, k, 8); // * 2^32
		// an f32 result narrows the f64 sum
		VReg sum = w == 8 ? vregFor(n) : fresh(kFp);
		farith(scalarEnc(0x58, 8), sum, scaled, dl, 8);
		if(w != 8)
			sse(scalarEnc(0x5a, 8), vr(vregFor(n), 4), {vr(sum)}); // cvtsd2ss
	}

	// x in an xmm or an x87 slot, k holds 2^63 alike. below 2^63 x converts as is, else x - 2^63
	// converts and gets its top bit back
	void X86LowerPass::emitFPToU64(ConvertNode* n, MachineOperand x, MachineOperand k) {
		B32 x87 = x.isFrameSlot();
		U32 w = x.width;
		X86Op toInt = X86Op::X87ToInt;
		X86Op compare = X86Op::X87Cmp;
		I64 desc = 0;
		if(!x87) {
			toInt = X86Op::Sse;
			compare = X86Op::FCmp;
			// cvttss2si, cvttsd2si
			desc = detail::packEnc(scalarEnc(0x2c, w, true), false);
		}
		VReg lo = fresh(kGp); // while x < 2^63
		put(toInt, {vr(lo)}, {x}, desc);
		MachineOperand biased;
		if(x87) {
			biased = sl(reserve(16));
			put(X86Op::X87Sub, {biased}, {x, k});
		} else {
			biased = vr(fresh(kFp), w);
			farith(scalarEnc(0x5c, w), biased.vreg, x.vreg, k.vreg, w); // subss, subsd
		}
		VReg hi = fresh(kGp);
		put(toInt, {vr(hi)}, {biased}, desc);
		alu(X86Op::Xor, hi, vr(gpConst((I64)0x8000000000000000ull))); // undo the bias
		// m = -(x >= 2^63)
		VReg m = fresh(kGp);
		put(compare, {vr(m)}, {x, k}, CC_AE);
		put(X86Op::Neg, {vr(m)}, {vr(m)});
		VReg d = vregFor(n); // d = lo ^ ((lo ^ hi) & m)
		mov(vr(d), vr(hi));
		alu(X86Op::Xor, d, vr(lo));
		alu(X86Op::And, d, vr(m));
		alu(X86Op::Xor, d, vr(lo));
	}

	void X86LowerPass::emitU64ToX87(ConvertNode* n, VReg s) {
		auto [hi, lo] = splitHalves(s);
		I32 fh = reserve(16);
		put(X86Op::X87FromInt, {sl(fh)}, {vr(hi)});
		I32 flo = reserve(16);
		put(X86Op::X87FromInt, {sl(flo)}, {vr(lo)});
		I32 k = reserve(16);
		put(X86Op::X87LoadImmD, {sl(k)}, {im((I64)0x41f0000000000000ull)}); // 2^32
		put(X86Op::X87Mul, {sl(fh)}, {sl(fh), sl(k)});
		put(X86Op::X87Add, {sl(x87SlotOf(n))}, {sl(fh), sl(flo)}); // exact in the 64-bit mantissa
	}

	void X86LowerPass::emitConvertX87(ConvertNode* n) {
		Node* src = n->getOperand();
		Opcode op = n->getOpcode();
		const Type* st = src->getType();
		I32 x = 0;
		VReg s = kNoVReg;
		if(isX87Ty(st))
			x = x87Value(src);
		else
			s = classValue(src, classOf(st));
		needScratch();
		switch(op) {
		case Opcode::FPExt:
			if(isX87Ty(st)) // long double -> long double is a plain move
				fldSlot(x87SlotOf(n), x);
			else
				put(X86Op::X87FromSse, {sl(x87SlotOf(n))}, {vr(s, opWidth(st))}, (I64)opWidth(st));
			return;
		case Opcode::FPTrunc: {
			U32 dw = opWidth(n->getType());
			put(X86Op::X87ToSse, {vr(vregFor(n), dw)}, {sl(x)}, (I64)dw);
			return;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP: {
			U32 bits = intBits(st);
			if(op == Opcode::UIToFP && bits >= 64)
				return emitU64ToX87(n, s);
			// fild reads all 64 bits
			VReg wide = widened(s, bits, op == Opcode::SIToFP);
			put(X86Op::X87FromInt, {sl(x87SlotOf(n))}, {vr(wide)});
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI:
			if(op == Opcode::FPToUI && intBits(n->getType()) >= 64) {
				I32 k = reserve(16);
				put(X86Op::X87LoadImmD, {sl(k)}, {im((I64)0x43e0000000000000ull)}); // 2^63
				return emitFPToU64(n, sl(x), sl(k));
			}
			put(X86Op::X87ToInt, {vr(vregFor(n))}, {sl(x)});
			return;
		default:
			return;
		}
	}
} // namespace rat
