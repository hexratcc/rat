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
	// narrow load used only by zext: load zero-extended, later zext is a no-op
	B32 X86LowerPass::zextOnlyLoad(const LoadNode* l) {
		const Type* t = l->getType();
		if(!t || !t->isInt() || intBits(t) >= 64)
			return false;
		if(l->getUsers().empty())
			return false;
		for(Node* u : l->getUsers()) {
			ConvertNode* cv = dyn_cast<ConvertNode>(u);
			if(!cv || cv->getOpcode() != Opcode::ZExt || cv->getOperand() != l)
				return false;
		}
		return true;
	}

	void X86LowerPass::emitStore(StoreNode* s) {
		Node* val = s->getValue();
		U32 w = opWidth(val->getType());
		if(isX87Ty(val->getType())) {
			VReg addr = gpValue(s->getPointer()); // x87 mem ops carry width in imm, no disp
			I32 s87 = x87Value(val);
			fstp(addr, slot(s87));
			return;
		}
		AddrParts a = matchAddr(s->getPointer());
		if(isSseTy(val->getType())) {
			VReg src = sseValue(val);
			stf(a, src, w);
			return;
		}
		if(ConstantNode* c = dyn_cast<ConstantNode>(val)) {
			I64 v = c->getValue();
			if(w < 8 || v == (I64)(I32)v) {
				st(a, MachineOperand::immVal(v, w));
				return;
			}
		}
		MachineOperand src = MachineOperand::vr(gpValue(val), w);
		// indexed reg store [base+index*scale+disp]=src has 3 gp uses; a vreg base lets all three
		// spill at once but only 2 gp scratch exist, so fold the address into an lea and keep the
		// store at <=2 gp uses
		if(a.hasIndex && !a.frameBase) {
			VReg t = fresh(detail::kGp);
			lea(t, a);
			st(t, std::move(src));
			return;
		}
		st(a, std::move(src));
	}

	void X86LowerPass::emitLoad(LoadNode* l) {
		U32 w = opWidth(l->getType());
		if(isX87Ty(l->getType())) {
			VReg addr = gpValue(l->getPointer());
			fld(slot(x87SlotOf(l)), addr);
			return;
		}
		AddrParts a = matchAddr(l->getPointer());
		if(isSseTy(l->getType())) {
			ldf(vregFor(l), w, a);
		} else {
			B32 sign = l->getType() && l->getType()->isInt() && !zextOnlyLoad(l);
			ld(vregFor(l), w, a, sign);
		}
	}

	void X86LowerPass::emitStackAlloc(Node* n) {
		VReg sz = gpValue(cast<StackAllocNode>(n)->getSize());
		stackAlloc(vregFor(n), sz);
	}

	void X86LowerPass::emitStackSave(Node* n) { stackSave(vregFor(n)); }

	void X86LowerPass::emitStackRestore(Node* n) {
		VReg sp = gpValue(cast<StackRestoreNode>(n)->getSaved());
		stackRestore(sp);
	}

	void X86LowerPass::twoAddr(X86Op op, VReg d, VReg lhs, VReg rhs) {
		mov(d, lhs);
		alu(op, d, d, rhs);
	}

	void X86LowerPass::maskBits(VReg d, U32 bits) {
		if(bits > 0 && bits < 64)
			maskBitsOp(d, bits);
	}

	void X86LowerPass::signExtBits(VReg d, U32 bits) {
		if(bits == 1) {
			maskBits(d, 1);
			return;
		}
		if(bits > 0 && bits < 64)
			signExtBitsOp(d, bits);
	}

	void X86LowerPass::emitDivLike(BinaryNode* n, X86Op op) {
		Opcode oc = n->getOpcode();
		B32 wantRem = (oc == Opcode::SRem || oc == Opcode::URem);
		VReg lhs = gpValue(n->getLHS());
		VReg rhs = gpValue(n->getRHS());
		VReg d = vregFor(n);

		U32 bits = intBits(n->getType());

		mov(R11, rhs);
		mov(RAX, lhs);
		mov(RCX, R11);

		idiv(op, bits);

		mov(d, wantRem ? RDX : RAX);
	}

	void X86LowerPass::emitShift(BinaryNode* n, X86Op op) {
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		U32 bits = intBits(n->getType());
		mov(d, lhs);
		if(op == X86Op::LShr)
			maskBits(d, bits);
		I64 iv;
		if(immOf(n->getRHS(), iv)) { // constant count: shift-by-imm, no RCX
			shift(op, d, imm(iv & 63));
		} else {
			VReg rhs = gpValue(n->getRHS());
			mov(RCX, rhs);
			shift(op, d, RCX);
		}
		if(op == X86Op::Shl)
			signExtBits(d, bits);
	}

	// rotates reach lowering only with a constant count (the fold rule builds
	// them that way)
	void X86LowerPass::emitRotate(BinaryNode* n, B32 left) {
		U32 bits = intBits(n->getType());
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		mov(d, lhs);
		I64 iv = 0;
		if(!immOf(n->getRHS(), iv))
			return; // degenerate; should not happen
		rot(left ? X86Op::Rotl : X86Op::Rotr, d, iv & (bits - 1), bits);
		signExtBits(d, bits); // restore in-register sign-extended convention
	}

	void X86LowerPass::emitBinary(BinaryNode* n) {
		Opcode op = n->getOpcode();
		if(n->getType() && n->getType()->isVec()) {
			emitVecBinary(n);
			return;
		}
		if(op >= Opcode::FAdd && op <= Opcode::FDiv) {
			emitFloatBinary(n);
			return;
		}
		switch(op) {
		case Opcode::SDiv:
		case Opcode::UDiv:
		case Opcode::SRem:
		case Opcode::URem: {
			static const X86Op kDiv[] = {X86Op::SDiv, X86Op::UDiv, X86Op::SRem, X86Op::URem};
			static_assert((U32)Opcode::URem - (U32)Opcode::SDiv + 1 == 4, "kDiv must cover SDiv..URem");
			emitDivLike(n, kDiv[(U32)op - (U32)Opcode::SDiv]);
			return;
		}
		case Opcode::Shl:
		case Opcode::LShr:
		case Opcode::AShr: {
			static const X86Op kShift[] = {X86Op::Shl, X86Op::LShr, X86Op::AShr};
			static_assert((U32)Opcode::AShr - (U32)Opcode::Shl + 1 == 3, "kShift must cover Shl..AShr");
			emitShift(n, kShift[(U32)op - (U32)Opcode::Shl]);
			return;
		}
		case Opcode::Rotl:
		case Opcode::Rotr: {
			emitRotate(n, op == Opcode::Rotl);
			return;
		}
		default:
			break;
		}
		X86Op mop;
		switch(op) {
			// clang-format off
		case Opcode::Add: mop = X86Op::Add; break;
		case Opcode::Sub: mop = X86Op::Sub; break;
		case Opcode::Mul: mop = X86Op::Mul; break;
		case Opcode::And: mop = X86Op::And; break;
		case Opcode::Or:  mop = X86Op::Or;  break;
		case Opcode::Xor: mop = X86Op::Xor; break;
		// clang-format on
		default:
			return;
		}
		Node* ln = n->getLHS();
		Node* rn = n->getRHS();
		I64 iv;
		if(op != Opcode::Sub && !immOf(rn, iv) && immOf(ln, iv))
			std::swap(ln, rn); // commutative ops: put a lone constant on the RHS
		VReg d = vregFor(n);
		U32 bits =
				(op == Opcode::Add || op == Opcode::Sub || op == Opcode::Mul) ? intBits(n->getType()) : 64u;
		if(immOf(rn, iv)) {
			VReg lhs = gpValue(ln);
			if(mop == X86Op::Mul) {
				// x*3, x*5, x*9
				U32 sc = 0;
				if(iv == 3)
					sc = 1;
				else if(iv == 5)
					sc = 2;
				else if(iv == 9)
					sc = 3;
				if(sc) {
					AddrParts a; // lhs + lhs*(1<<sc)
					a.base = lhs;
					a.index = lhs;
					a.scaleLog2 = sc;
					a.hasIndex = true;
					lea(d, a);
					signExtBits(d, bits);
					return;
				}
				// otherwise three-operand imul: no tied copy needed
				imul(d, lhs, imm(iv));
				signExtBits(d, bits);
				return;
			}
			mov(d, lhs);
			alu(mop, d, d, imm(iv));
			signExtBits(d, bits);
			return;
		}
		VReg lhs = gpValue(ln);
		VReg rhs = gpValue(rn);
		twoAddr(mop, d, lhs, rhs);
		signExtBits(d, bits);
	}

	void X86LowerPass::emitFloatBinary(BinaryNode* n) {
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::FAdd; // 0..3
		if(isX87Ty(n->getType())) {
			emitX87Binary(n, idx);
			return;
		}
		U32 w = opWidth(n->getType());
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		static const X86Op kFOps[] = {X86Op::FAdd, X86Op::FSub, X86Op::FMul, X86Op::FDiv};
		farith(kFOps[idx], d, lhs, rhs, w, (I64)w);
	}

	void X86LowerPass::needVecScratch() {
		if(fl->vecScratch == 0)
			fl->vecScratch = reserve(16);
	}

	String X86LowerPass::vecPoolSym(const List<U8>& bytes) {
		C8 buf[48];
		U64 h = 1469598103934665603ull;
		for(U8 x : bytes) {
			h ^= x;
			h *= 1099511628211ull;
		}
		std::snprintf(buf, sizeof buf, "__rat_vec_%016lx", (U64)h);
		String name(buf);
		if(!mod->getGlobal(name)) {
			List<U8> init = bytes;
			Global* g = mod->createGlobal(name, mod->getArray(mod->getInt(8), 16), true, std::move(init));
			g->setLinkage(Global::Linkage::Internal);
		}
		return name;
	}

	void X86LowerPass::emitVecBinary(BinaryNode* n) {
		Type* t = n->getType();
		Type* et = t->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		Opcode op = n->getOpcode();

		U8 pfx = 0, opc = 0;
		B32 esc38 = false;
		if(et->isInt()) {
			pfx = 0x66;
			// clang-format off
			switch(op) {
			case Opcode::Add: opc = esz == 4 ? 0xfe : 0xd4; break; // paddd / paddq
			case Opcode::Sub: opc = esz == 4 ? 0xfa : 0xfb; break; // psubd / psubq
			case Opcode::And: opc = 0xdb; break;									 // pand
			case Opcode::Or: opc = 0xeb; break;										 // por
			case Opcode::Xor: opc = 0xef; break;									 // pxor
			case Opcode::Mul: // pmulld, i32 lanes only
				if(esz != 4)
					return;
				opc = 0x40;
				esc38 = true;
				break;
			default: return;
			}
			// clang-format on
		} else {
			pfx = esz == 8 ? 0x66 : 0; // addpd... vs addps...
																 // clang-format off
			switch(op) {
			case Opcode::FAdd: opc = 0x58; break;
			case Opcode::FSub: opc = 0x5c; break;
			case Opcode::FMul: opc = 0x59; break;
			case Opcode::FDiv: opc = 0x5e; break;
			default: return;
			}
																 // clang-format on
		}

		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		varith(d, lhs, rhs, pfx, opc, esc38);
	}

	void X86LowerPass::emitSplat(SplatNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		B32 isInt = !et->isFloat();
		VReg s = isInt ? gpValue(n->getScalar()) : sseValue(n->getScalar());
		vsplat(vregFor(n), s, esz, isInt);
	}

	void X86LowerPass::emitShuffle(ShuffleNode* n) {
		VReg v = sseValue(n->getVector());
		vshuf(vregFor(n), v, n->getSelector());
	}

	void X86LowerPass::emitExtract(ExtractNode* n) {
		Type* et = n->getType();
		U32 esz = et->byteSize(ptrBytes);
		B32 isInt = !et->isFloat();
		VReg v = sseValue(n->getVector());
		if(isInt && n->getLane() != 0)
			needVecScratch(); // staged through memory
		vextract(vregFor(n), v, n->getLane(), esz, isInt);
	}

	void X86LowerPass::emitPack(PackNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		U32 w = n->getLaneCount();
		B32 isInt = !et->isFloat();

		// all-constant packs come from the constant pool as one movups
		B32 allConst = true;
		for(U32 i = 0; i < w; ++i)
			allConst &= isa<ConstantNode>(n->getLane(i));
		if(allConst) {
			List<U8> bytes(16, 0);
			for(U32 i = 0; i < w; ++i) {
				U64 v = (U64)cast<ConstantNode>(n->getLane(i))->getValue();
				for(U32 b = 0; b < esz; ++b)
					bytes[i * esz + b] = (U8)(v >> (8 * b));
			}
			ldf(vregFor(n), 16, vecPoolSym(bytes));
			return;
		}

		// int lanes with sse4.1 build the vector in-register (VPackReg); float and pre-sse4.1
		// fall back to gathering through the vec scratch slot (VPack)
		B32 useReg = isInt && sse41;
		X86Op op = X86Op::VPack;
		if(useReg)
			op = X86Op::VPackReg;
		else
			needVecScratch();
		List<MachineOperand> lanes;
		for(U32 i = 0; i < w; ++i) {
			Node* lane = n->getLane(i);
			if(isInt)
				lanes.push_back(MachineOperand::vr(gpValue(lane)));
			else
				lanes.push_back(MachineOperand::vr(sseValue(lane), esz));
		}
		vpack(op, vregFor(n), std::move(lanes), esz, isInt);
	}

	void X86LowerPass::emitX87Binary(BinaryNode* n, U32 idx) {
		static const X86Op kOps[] = {X86Op::X87Add, X86Op::X87Sub, X86Op::X87Mul, X86Op::X87Div};
		I32 lhs = x87Value(n->getLHS());
		I32 rhs = x87Value(n->getRHS());
		x87Arith(kOps[idx], slot(x87SlotOf(n)), slot(lhs), slot(rhs));
	}

	// d OP= s
	void X86LowerPass::gpAcc(X86Op op, VReg d, VReg s) { alu(op, d, d, s); }

	// d >>= cnt, logical
	void X86LowerPass::gpShrImm(VReg d, U32 cnt) { shift(X86Op::LShr, d, imm((I64)cnt)); }

	// a fresh register holding a constant too wide for an ALU immediate
	VReg X86LowerPass::gpConst(I64 v) {
		VReg t = fresh(detail::kGp);
		movi(t, v);
		return t;
	}

	void X86LowerPass::emitBitScan(UnaryNode* n, B32 reverse) {
		U32 bits = intBits(n->getType());
		U32 w = bits > 32 ? 64u : 32u;
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		if(bits < w) {
			mov(d, s);
			maskBits(d, bits);
			s = d;
		}
		bitScan(reverse ? X86Op::BitScanR : X86Op::BitScanF, d, s, w);
		if(reverse)
			alu(X86Op::Xor, d, d, imm((I64)bits - 1));
	}

	void X86LowerPass::emitPopcnt(UnaryNode* n) {
		static const I64 k55 = (I64)0x5555555555555555ull;
		static const I64 k33 = (I64)0x3333333333333333ull;
		static const I64 k0f = (I64)0x0f0f0f0f0f0f0f0full;
		static const I64 k01 = (I64)0x0101010101010101ull;
		U32 bits = intBits(n->getType());
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		VReg t = fresh(detail::kGp);
		mov(d, s);
		maskBits(d, bits); // keep only its bits
		// d -= (d >> 1) & k55
		mov(t, d);
		gpShrImm(t, 1);
		gpAcc(X86Op::And, t, gpConst(k55));
		gpAcc(X86Op::Sub, d, t);
		// d = (d & k33) + ((d >> 2) & k33)
		VReg m33 = gpConst(k33);
		mov(t, d);
		gpShrImm(t, 2);
		gpAcc(X86Op::And, t, m33);
		gpAcc(X86Op::And, d, m33);
		gpAcc(X86Op::Add, d, t);
		// d = (d + (d >> 4)) & k0f
		mov(t, d);
		gpShrImm(t, 4);
		gpAcc(X86Op::Add, d, t);
		gpAcc(X86Op::And, d, gpConst(k0f));
		// every byte now holds its own count
		gpAcc(X86Op::Mul, d, gpConst(k01));
		gpShrImm(d, 56);
	}

	void X86LowerPass::emitBswap(UnaryNode* n) {
		U32 bits = intBits(n->getType());
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		mov(d, s);
		bswap(d, bits > 32 ? 64 : 32);
		signExtBits(d, bits); // bswap r32 zero-extends
	}

	void X86LowerPass::emitUnary(UnaryNode* n) {
		Opcode uop = n->getOpcode();
		if(uop == Opcode::Bswap) {
			emitBswap(n);
			return;
		}
		if(uop == Opcode::Clz || uop == Opcode::Ctz) {
			emitBitScan(n, uop == Opcode::Clz);
			return;
		}
		if(uop == Opcode::Popcnt) {
			emitPopcnt(n);
			return;
		}
		if(n->getOpcode() == Opcode::FNeg) {
			if(isX87Ty(n->getType())) {
				I32 s = x87Value(n->getOperand());
				fchs(slot(x87SlotOf(n)), slot(s));
				return;
			}
			U32 w = opWidth(n->getType());
			VReg s = sseValue(n->getOperand());
			needScratch();
			fneg(vregFor(n), s, w);
			return;
		}
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		B32 isNeg = uop == Opcode::Neg;
		mov(d, s);
		if(isNeg) {
			neg(d);
			signExtBits(d, intBits(n->getType())); // -INT_MIN carries out of the width
		} else {
			not_(d);
		}
	}

	// flag-setting cmp only; the caller emits its own jcc/setcc consumer
	// cmp lhs, rhs; returns the cc that tests the compare
	U8 X86LowerPass::emitIntCmp(CompareNode* n) {
		VReg lhs = gpValue(n->getLHS());
		I64 iv;
		if(immOf(n->getRHS(), iv)) {
			cmp(lhs, imm(iv));
		} else {
			VReg rhs = gpValue(n->getRHS());
			cmp(lhs, rhs);
		}
		return detail::kIntCc[(U32)n->getOpcode() - (U32)Opcode::Eq];
	}

	// ucomis lhs, rhs (swap keeps lt/le NaN-correct); returns the cc that tests the compare
	U8 X86LowerPass::fusedFpCmp(CompareNode* n) {
		U32 w = opWidth(n->getLHS()->getType());
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::FEq;
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		ucomisFlags(lhs, rhs, w, detail::kFpSwap[idx]);
		return detail::kFpCc[idx];
	}

	// dst starts as the else-value, then cmov overwrites it when the flags say so
	// the condition folds into the cmp when it is an integer compare used only here
	void X86LowerPass::emitSelect(SelectNode* n) {
		Node* cond = n->getCondition();
		VReg d = vregFor(n);
		// materialize both arms before the compare
		VReg f = gpValue(n->getFalse());
		VReg t = gpValue(n->getTrue());
		mov(d, f);

		U8 cc = CC_NE;
		if(selectOnlyCompare(cond)) {
			cc = emitIntCmp(cast<CompareNode>(cond));
		} else if(fpSelectOnlyCompare(cond)) {
			cc = fusedFpCmp(cast<CompareNode>(cond));
		} else {
			VReg cv = gpValue(cond);
			cmp(cv, imm(0));
		}
		cmov(d, t, cc);
	}

	void X86LowerPass::emitCompare(CompareNode* n) {
		Opcode op = n->getOpcode();
		if(op >= Opcode::FEq && op <= Opcode::FGe) {
			emitFloatCompare(n);
			return;
		}
		U8 cc = emitIntCmp(n);
		VReg d = vregFor(n);
		setcc(d, cc);
	}

	void X86LowerPass::emitFloatCompare(CompareNode* n) {
		U32 fcIdx = (U32)n->getOpcode() - (U32)Opcode::FEq;
		U8 cc = detail::kFpCc[fcIdx];
		I64 swap = detail::kFpSwap[fcIdx] ? 1 : 0;
		VReg d = vregFor(n);
		if(isX87Ty(n->getLHS()->getType())) {
			I32 lhs = x87Value(n->getLHS());
			I32 rhs = x87Value(n->getRHS());
			fucomi(d, slot(lhs), slot(rhs), cc, swap != 0);
			unorderedFixup(n->getOpcode(), d);
			return;
		}
		U32 w = opWidth(n->getLHS()->getType());
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		ucomis(d, lhs, rhs, w, cc, swap != 0);
		unorderedFixup(n->getOpcode(), d);
	}

	void X86LowerPass::unorderedFixup(Opcode op, VReg d) {
		if(op != Opcode::FEq && op != Opcode::FNe)
			return;
		VReg t = fresh(detail::kGp);
		setcc(t, op == Opcode::FEq ? CC_NP : CC_P);
		gpAcc(op == Opcode::FEq ? X86Op::And : X86Op::Or, d, t);
	}

	I64 X86LowerPass::cvtDesc(U8 pfx, U8 opc, B32 w) {
		return ((I64)pfx << 16) | ((I64)opc << 8) | (w ? 1 : 0);
	}

	void X86LowerPass::emitConvert(ConvertNode* n) {
		Node* src = n->getOperand();
		Opcode op = n->getOpcode();
		if(isX87Ty(n->getType()) || isX87Ty(src->getType())) {
			emitConvertX87(n, src, op);
			return;
		}
		switch(op) {
		case Opcode::Trunc:
		case Opcode::SExt:
		case Opcode::ZExt: {
			// values live sign-extended in 64-bit registers
			VReg s = gpValue(src);
			VReg d = vregFor(n);
			mov(d, s);
			if(op == Opcode::Trunc) {
				signExtBits(d, intBits(n->getType()));
			} else if(op == Opcode::ZExt) {
				// already zero-extended when the source load emitted movzx
				LoadNode* ld = dyn_cast<LoadNode>(src);
				if(!ld || !zextOnlyLoad(ld))
					maskBits(d, intBits(src->getType()));
			}
			return;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP: {
			U32 w = opWidth(n->getType());
			U32 sb = intBits(src->getType());
			VReg s = gpValue(src);
			if(op == Opcode::UIToFP && sb >= 64) {
				emitU64ToFP(n, s, w);
				return;
			}
			if(op == Opcode::UIToFP) {
				VReg z = fresh(detail::kGp);
				mov(z, s);
				maskBits(z, sb);
				s = z;
			}
			cvtf(vregFor(n), w, s, 8, Asm::ssePrefixByte(w), 0x2a, true);
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			if(op == Opcode::FPToUI && intBits(n->getType()) >= 64) {
				emitFPToU64(n, src);
				return;
			}
			U32 w = opWidth(src->getType());
			VReg s = sseValue(src);
			VReg d = vregFor(n);
			B32 wide = op == Opcode::FPToUI || intBits(n->getType()) > 32;
			cvti(d, s, w, Asm::ssePrefixByte(w), 0x2c, wide);
			signExtBits(d, intBits(n->getType())); // cvtt writes a whole register; restore the convention
			return;
		}
		case Opcode::FPExt: { // f32 -> f64: cvtss2sd
			VReg s = sseValue(src);
			cvtf(vregFor(n), 8, s, 4, 0xf3, 0x5a, false);
			return;
		}
		case Opcode::FPTrunc: { // f64 -> f32: cvtsd2ss
			VReg s = sseValue(src);
			cvtf(vregFor(n), 4, s, 8, 0xf2, 0x5a, false);
			return;
		}
		default:
			return;
		}
	}

	void X86LowerPass::emitU64ToFP(ConvertNode* n, VReg s, U32 w) {
		VReg hi = fresh(detail::kGp);
		mov(hi, s);
		gpShrImm(hi, 32);
		VReg lo = fresh(detail::kGp);
		mov(lo, s);
		maskBits(lo, 32);

		U8 pfx = Asm::ssePrefixByte(8); // cvtsi2sd
		VReg dh = fresh(detail::kFp);
		cvtf(dh, 8, hi, 8, pfx, 0x2a, true);
		VReg dl = fresh(detail::kFp);
		cvtf(dl, 8, lo, 8, pfx, 0x2a, true);

		VReg scaled = fresh(detail::kFp);
		farith(X86Op::FMul, scaled, dh, fpConst(0x41f0000000000000ull, 8), 8, 8); // * 2^32
		if(w == 8) {
			farith(X86Op::FAdd, vregFor(n), scaled, dl, 8, 8);
			return;
		}
		VReg sum = fresh(detail::kFp);
		farith(X86Op::FAdd, sum, scaled, dl, 8, 8);
		cvtf(vregFor(n), 4, sum, 8, 0xf2, 0x5a, false); // cvtsd2ss
	}

	void X86LowerPass::emitFPToU64(ConvertNode* n, Node* src) {
		U32 w = opWidth(src->getType());
		VReg x = sseValue(src);
		VReg k = fpConst(w == 4 ? 0x5f000000ull : 0x43e0000000000000ull, w); // 2^63
		U8 pfx = Asm::ssePrefixByte(w);

		VReg lo = fresh(detail::kGp); // while x < 2^63
		cvti(lo, x, w, pfx, 0x2c, true);

		VReg biased = fresh(detail::kFp);
		farith(X86Op::FSub, biased, x, k, w, (I64)w);
		VReg hi = fresh(detail::kGp);
		cvti(hi, biased, w, pfx, 0x2c, true);
		gpAcc(X86Op::Xor, hi, gpConst((I64)0x8000000000000000ull)); // undo the bias

		VReg m = fresh(detail::kGp); // -(x >= 2^63)
		ucomis(m, x, k, w, CC_AE, false);
		neg(m);

		VReg d = vregFor(n); // d = lo ^ ((lo ^ hi) & m)
		mov(d, hi);
		gpAcc(X86Op::Xor, d, lo);
		gpAcc(X86Op::And, d, m);
		gpAcc(X86Op::Xor, d, lo);
	}

	void X86LowerPass::emitConvertX87(ConvertNode* n, Node* src, Opcode op) {
		switch(op) {
		case Opcode::FPExt: {
			if(isX87Ty(src->getType())) { // long double -> long double: plain move
				I32 s = x87Value(src);
				needScratch();
				x87Move(x87SlotOf(n), s);
				return;
			}
			U32 sw = opWidth(src->getType());
			VReg s = sseValue(src);
			needScratch();
			fldSse(slot(x87SlotOf(n)), s, sw);
			return;
		}
		case Opcode::FPTrunc: {
			I32 s = x87Value(src);
			U32 dw = opWidth(n->getType());
			needScratch();
			fstpSse(vregFor(n), dw, slot(s));
			return;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP: {
			VReg s = gpValue(src);
			needScratch();
			fild(slot(x87SlotOf(n)), s);
			return;
		}
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			I32 s = x87Value(src);
			needScratch();
			fistp(vregFor(n), slot(s));
			return;
		}
		default:
			return;
		}
	}

} // namespace rat
