#include "pass/emit/x86/x86_lower.h"

#include "codegen/machine_function.h"
#include "hash.h"
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
		if(!t || !t->isInt() || intBits(t) >= 64 || l->getUsers().empty())
			return false;
		for(Node* u : l->getUsers())
			if(u->getOpcode() != Opcode::ZExt)
				return false;
		return true;
	}

	void X86LowerPass::emitStore(StoreNode* s) {
		Node* val = s->getValue();
		U32 w = opWidth(val->getType());
		if(isX87Ty(val->getType())) {
			VReg addr = gpValue(s->getPointer()); // x87 mem ops carry width in imm, no disp
			fstp(addr, x87Value(val));
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
			fld(x87SlotOf(l), addr);
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
		VReg lhs = gpValue(n->getLHS());
		VReg rhs = gpValue(n->getRHS());
		VReg d = vregFor(n);
		mov(R11, rhs);
		mov(RAX, lhs);
		mov(RCX, R11);
		idiv(op, intBits(n->getType()));
		B32 wantRem = op == X86Op::SRem || op == X86Op::URem;
		mov(d, wantRem ? RDX : RAX);
	}

	void X86LowerPass::emitShift(BinaryNode* n, X86Op op) {
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		U32 bits = intBits(n->getType());
		mov(d, lhs);
		if(op == X86Op::LShr)
			maskBits(d, bits);
		I64 iv = 0;
		B32 immCnt = immOf(n->getRHS(), iv);
		if(immCnt) { // constant count: shift-by-imm, no RCX
			shift(op, d, imm(iv & 63));
		} else {
			VReg rhs = gpValue(n->getRHS());
			mov(RCX, rhs);
			shift(op, d, RCX);
		}
		B32 cntMayBeZero = !immCnt || (iv & 63) == 0;
		if(op == X86Op::Shl || (op == X86Op::LShr && cntMayBeZero))
			signExtBits(d, bits);
	}

	// rotates reach lowering only with a constant count (the fold rule builds
	// them that way)
	void X86LowerPass::emitRotate(BinaryNode* n, X86Op op) {
		U32 bits = intBits(n->getType());
		VReg lhs = gpValue(n->getLHS());
		VReg d = vregFor(n);
		mov(d, lhs);
		I64 iv = 0;
		if(!immOf(n->getRHS(), iv))
			return; // degenerate; should not happen
		rot(op, d, iv & (bits - 1), bits);
		signExtBits(d, bits); // restore in-register sign-extended convention
	}

	void X86LowerPass::emitBinary(BinaryNode* n) {
		// clang-format off
		static const X86Op kOps[] = {X86Op::Add, X86Op::Sub, X86Op::Mul, X86Op::SDiv, X86Op::UDiv,
			X86Op::SRem, X86Op::URem, X86Op::And, X86Op::Or, X86Op::Xor, X86Op::Shl, X86Op::LShr,
			X86Op::AShr, X86Op::Rotl, X86Op::Rotr};
		// clang-format on
		static_assert((U32)Opcode::Rotr - (U32)Opcode::Add + 1 == 15, "kOps must cover Add..Rotr");
		Opcode op = n->getOpcode();
		if(n->getType() && n->getType()->isVec()) {
			emitVecBinary(n);
			return;
		}
		if(op >= Opcode::FAdd && op <= Opcode::FDiv) {
			emitFloatBinary(n);
			return;
		}
		if(op < Opcode::Add || op > Opcode::Rotr)
			return;
		X86Op mop = kOps[(U32)op - (U32)Opcode::Add];
		if(op >= Opcode::SDiv && op <= Opcode::URem)
			emitDivLike(n, mop);
		else if(op >= Opcode::Shl && op <= Opcode::AShr)
			emitShift(n, mop);
		else if(op >= Opcode::Rotl)
			emitRotate(n, mop);
		else
			emitAlu(n, mop);
	}

	void X86LowerPass::emitAlu(BinaryNode* n, X86Op op) {
		Node* ln = n->getLHS();
		Node* rn = n->getRHS();
		I64 iv;
		if(op != X86Op::Sub && !immOf(rn, iv) && immOf(ln, iv))
			std::swap(ln, rn); // commutative ops: put a lone constant on the RHS
		VReg d = vregFor(n);
		VReg lhs = gpValue(ln);
		if(!immOf(rn, iv)) {
			VReg rhs = gpValue(rn);
			mov(d, lhs);
			alu(op, d, d, rhs);
		} else if(op == X86Op::Mul) {
			mulImm(d, lhs, iv);
		} else {
			mov(d, lhs);
			alu(op, d, d, imm(iv));
		}
		if(op == X86Op::Add || op == X86Op::Sub || op == X86Op::Mul)
			signExtBits(d, intBits(n->getType()));
	}

	// x*3, x*5, x*9
	void X86LowerPass::mulImm(VReg d, VReg s, I64 v) {
		U32 sc = log2Scale(v - 1);
		if(sc == 0) {
			// otherwise three-operand imul: no tied copy needed
			imul(d, s, imm(v));
			return;
		}
		AddrParts a; // lhs + lhs*(1<<sc)
		a.base = s;
		a.index = s;
		a.scaleLog2 = sc;
		a.hasIndex = true;
		lea(d, a);
	}

	void X86LowerPass::emitFloatBinary(BinaryNode* n) {
		U32 idx = (U32)n->getOpcode() - (U32)Opcode::FAdd; // 0..3
		if(isX87Ty(n->getType())) {
			static const X86Op kX87[] = {X86Op::X87Add, X86Op::X87Sub, X86Op::X87Mul, X86Op::X87Div};
			Slot lhs = x87Value(n->getLHS());
			Slot rhs = x87Value(n->getRHS());
			x87Arith(kX87[idx], x87SlotOf(n), lhs, rhs);
			return;
		}
		U32 w = opWidth(n->getType());
		VReg lhs = sseValue(n->getLHS());
		VReg rhs = sseValue(n->getRHS());
		VReg d = vregFor(n);
		static const X86Op kFOps[] = {X86Op::FAdd, X86Op::FSub, X86Op::FMul, X86Op::FDiv};
		farith(kFOps[idx], d, lhs, rhs, w, (I64)w);
	}

	const String& X86LowerPass::vecPoolSym(const List<U8>& bytes) {
		C8 buf[48];
		U64 h = kFnvBasis;
		for(U8 x : bytes)
			hashMix(h, x);
		std::snprintf(buf, sizeof buf, "__rat_vec_%016lx", (U64)h);
		String name(buf);
		Global* g = mod->getGlobal(name);
		if(!g) {
			List<U8> init = bytes;
			g = mod->createGlobal(name, mod->getArray(mod->getInt(8), 16), true, std::move(init));
			g->setLinkage(Global::Linkage::Internal);
		}
		return g->getName(); // outlives the machine code that names it
	}

	void X86LowerPass::emitVecBinary(BinaryNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		Opcode op = n->getOpcode();
		U8 pfx = 0, opc = 0;
		B32 esc38 = false;
		// clang-format off
		if(et->isInt()) {
			pfx = 0x66;
			switch(op) {
			case Opcode::Add: opc = esz == 4 ? 0xfe : 0xd4; break; // paddd / paddq
			case Opcode::Sub: opc = esz == 4 ? 0xfa : 0xfb; break; // psubd / psubq
			case Opcode::And: opc = 0xdb; break; // pand
			case Opcode::Or: opc = 0xeb; break;  // por
			case Opcode::Xor: opc = 0xef; break; // pxor
			case Opcode::Mul: // pmulld, i32 lanes only
				if(esz != 4)
					return;
				opc = 0x40;
				esc38 = true;
				break;
			default: return;
			}
		} else {
			pfx = esz == 8 ? 0x66 : 0; // addpd... vs addps...
			switch(op) {
			case Opcode::FAdd: opc = 0x58; break;
			case Opcode::FSub: opc = 0x5c; break;
			case Opcode::FMul: opc = 0x59; break;
			case Opcode::FDiv: opc = 0x5e; break;
			default: return;
			}
		}
		// clang-format on
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

	void X86LowerPass::emitExtract(ExtractNode* n) {
		Type* et = n->getType();
		U32 esz = et->byteSize(ptrBytes);
		B32 isInt = !et->isFloat();
		VReg v = sseValue(n->getVector());
		if(isInt && n->getLane() != 0 && fl->vecScratch == 0)
			fl->vecScratch = reserve(16); // staged through memory
		vextract(vregFor(n), v, n->getLane(), esz, isInt);
	}

	// all-constant packs come from the constant pool as one movups
	B32 X86LowerPass::emitConstPack(PackNode* n, U32 esz) {
		U32 w = n->getLaneCount();
		for(U32 i = 0; i < w; ++i)
			if(!isa<ConstantNode>(n->getLane(i)))
				return false;
		List<U8> bytes(16, 0);
		for(U32 i = 0; i < w; ++i) {
			U64 v = (U64)cast<ConstantNode>(n->getLane(i))->getValue();
			for(U32 b = 0; b < esz; ++b)
				bytes[i * esz + b] = (U8)(v >> (8 * b));
		}
		ldf(vregFor(n), 16, vecPoolSym(bytes));
		return true;
	}

	void X86LowerPass::emitPack(PackNode* n) {
		Type* et = n->getType()->getVecElement();
		U32 esz = et->byteSize(ptrBytes);
		U32 w = n->getLaneCount();
		B32 isInt = !et->isFloat();
		if(emitConstPack(n, esz))
			return;
		// int lanes with sse4.1 build the vector in-register (VPackReg); float and pre-sse4.1
		// fall back to gathering through the vec scratch slot (VPack)
		B32 useReg = isInt && sse41;
		if(!useReg && fl->vecScratch == 0)
			fl->vecScratch = reserve(16);
		List<MachineOperand> lanes;
		for(U32 i = 0; i < w; ++i) {
			Node* lane = n->getLane(i);
			if(isInt)
				lanes.push_back(MachineOperand::vr(gpValue(lane)));
			else
				lanes.push_back(MachineOperand::vr(sseValue(lane), esz));
		}
		if(useReg)
			vpackReg(vregFor(n), lanes, esz);
		else
			vpackMem(vregFor(n), lanes, esz, isInt);
	}

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
		shift(X86Op::LShr, t, imm(1));
		alu(X86Op::And, t, t, gpConst(k55));
		alu(X86Op::Sub, d, d, t);
		// d = (d & k33) + ((d >> 2) & k33)
		VReg m33 = gpConst(k33);
		mov(t, d);
		shift(X86Op::LShr, t, imm(2));
		alu(X86Op::And, t, t, m33);
		alu(X86Op::And, d, d, m33);
		alu(X86Op::Add, d, d, t);
		// d = (d + (d >> 4)) & k0f
		mov(t, d);
		shift(X86Op::LShr, t, imm(4));
		alu(X86Op::Add, d, d, t);
		alu(X86Op::And, d, d, gpConst(k0f));
		// every byte now holds its own count
		alu(X86Op::Mul, d, d, gpConst(k01));
		shift(X86Op::LShr, d, imm(56));
	}

	void X86LowerPass::emitFNeg(UnaryNode* n) {
		if(isX87Ty(n->getType())) {
			Slot s = x87Value(n->getOperand());
			fchs(x87SlotOf(n), s);
			return;
		}
		U32 w = opWidth(n->getType());
		VReg s = sseValue(n->getOperand());
		needScratch();
		fneg(vregFor(n), s, w);
	}

	void X86LowerPass::emitUnary(UnaryNode* n) {
		Opcode uop = n->getOpcode();
		if(uop == Opcode::Clz || uop == Opcode::Ctz) {
			emitBitScan(n, uop == Opcode::Clz);
			return;
		}
		if(uop == Opcode::Popcnt) {
			emitPopcnt(n);
			return;
		}
		if(uop == Opcode::FNeg) {
			emitFNeg(n);
			return;
		}
		U32 bits = intBits(n->getType());
		VReg s = gpValue(n->getOperand());
		VReg d = vregFor(n);
		mov(d, s);
		if(uop == Opcode::Bswap) {
			bswap(d, bits > 32 ? 64 : 32);
			signExtBits(d, bits); // bswap r32 zero-extends
		} else if(uop == Opcode::Neg) {
			neg(d);
			signExtBits(d, bits); // -INT_MIN carries out of the width
		} else {
			not_(d);
		}
	}

	// flag-setting cmp only; the caller emits its own jcc/setcc consumer
	// cmp lhs, rhs; returns the cc that tests the compare
	U8 X86LowerPass::emitCmp(CompareNode* n) {
		if(n->getOpcode() < Opcode::FEq) {
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
			cc = emitCmp(cast<CompareNode>(cond));
		} else {
			VReg cv = gpValue(cond);
			cmp(cv, imm(0));
		}
		cmov(d, t, cc);
	}

	void X86LowerPass::emitCompare(CompareNode* n) {
		Opcode op = n->getOpcode();
		if(n->getOpcode() < Opcode::FEq) {
			U8 cc = emitCmp(n);
			VReg d = vregFor(n);
			setcc(d, cc);
			return;
		}
		U32 idx = (U32)op - (U32)Opcode::FEq;
		U8 cc = detail::kFpCc[idx];
		B32 swap = detail::kFpSwap[idx] != 0;
		VReg d = vregFor(n);
		if(isX87Ty(n->getLHS()->getType())) {
			Slot lhs = x87Value(n->getLHS());
			Slot rhs = x87Value(n->getRHS());
			fucomi(d, lhs, rhs, cc, swap);
		} else {
			U32 w = opWidth(n->getLHS()->getType());
			VReg lhs = sseValue(n->getLHS());
			VReg rhs = sseValue(n->getRHS());
			ucomis(d, lhs, rhs, w, cc, swap);
		}
		if(op != Opcode::FEq && op != Opcode::FNe)
			return;
		VReg t = fresh(detail::kGp);
		setcc(t, op == Opcode::FEq ? CC_NP : CC_P);
		alu(op == Opcode::FEq ? X86Op::And : X86Op::Or, d, d, t);
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
		case Opcode::ZExt:
			emitIntResize(n, src);
			return;
		case Opcode::SIToFP:
		case Opcode::UIToFP:
			emitIntToFP(n, src);
			return;
		case Opcode::FPToSI:
		case Opcode::FPToUI:
			emitFPToInt(n, src);
			return;
		case Opcode::FPExt:			// f32 -> f64: cvtss2sd
		case Opcode::FPTrunc: { // f64 -> f32: cvtsd2ss
			U32 sw = opWidth(src->getType());
			VReg s = sseValue(src);
			cvtf(vregFor(n), opWidth(n->getType()), s, sw, Asm::ssePrefixByte(sw), 0x5a, false);
			return;
		}
		default:
			return;
		}
	}

	// values live sign-extended in 64-bit registers
	void X86LowerPass::emitIntResize(ConvertNode* n, Node* src) {
		Opcode op = n->getOpcode();
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
	}

	void X86LowerPass::emitIntToFP(ConvertNode* n, Node* src) {
		B32 isUnsigned = n->getOpcode() == Opcode::UIToFP;
		U32 w = opWidth(n->getType());
		U32 sb = intBits(src->getType());
		VReg s = gpValue(src);
		if(isUnsigned && sb >= 64) {
			emitU64ToFP(n, s, w);
			return;
		}
		if(isUnsigned) {
			VReg z = fresh(detail::kGp);
			mov(z, s);
			maskBits(z, sb);
			s = z;
		}
		cvtf(vregFor(n), w, s, 8, Asm::ssePrefixByte(w), 0x2a, true);
	}

	void X86LowerPass::emitFPToInt(ConvertNode* n, Node* src) {
		B32 isUnsigned = n->getOpcode() == Opcode::FPToUI;
		U32 bits = intBits(n->getType());
		if(isUnsigned && bits >= 64) {
			emitFPToU64(n, src);
			return;
		}
		U32 w = opWidth(src->getType());
		VReg s = sseValue(src);
		VReg d = vregFor(n);
		cvti(d, s, w, Asm::ssePrefixByte(w), 0x2c, isUnsigned || bits > 32);
		signExtBits(d, bits); // cvtt writes a whole register; restore the convention
	}

	Pair<VReg, VReg> X86LowerPass::splitHalves(VReg s) {
		VReg hi = fresh(detail::kGp);
		mov(hi, s);
		shift(X86Op::LShr, hi, imm(32));
		VReg lo = fresh(detail::kGp);
		mov(lo, s);
		maskBits(lo, 32);
		return {hi, lo};
	}

	void X86LowerPass::blendU64(ConvertNode* n, VReg lo, VReg hi, VReg m) {
		neg(m);
		VReg d = vregFor(n); // d = lo ^ ((lo ^ hi) & m)
		mov(d, hi);
		alu(X86Op::Xor, d, d, lo);
		alu(X86Op::And, d, d, m);
		alu(X86Op::Xor, d, d, lo);
	}

	void X86LowerPass::emitU64ToFP(ConvertNode* n, VReg s, U32 w) {
		auto [hi, lo] = splitHalves(s);
		U8 pfx = Asm::ssePrefixByte(8); // cvtsi2sd
		VReg dh = fresh(detail::kFp);
		cvtf(dh, 8, hi, 8, pfx, 0x2a, true);
		VReg dl = fresh(detail::kFp);
		cvtf(dl, 8, lo, 8, pfx, 0x2a, true);

		VReg scaled = fresh(detail::kFp);
		VReg k = fresh(detail::kFp);
		ldf(k, 8, fpPoolSym(0x41f0000000000000ull, 8));
		farith(X86Op::FMul, scaled, dh, k, 8, 8); // * 2^32
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
		VReg k = fresh(detail::kFp);
		ldf(k, w, fpPoolSym(w == 4 ? 0x5f000000ull : 0x43e0000000000000ull, w)); // 2^63
		U8 pfx = Asm::ssePrefixByte(w);

		VReg lo = fresh(detail::kGp); // while x < 2^63
		cvti(lo, x, w, pfx, 0x2c, true);

		VReg biased = fresh(detail::kFp);
		farith(X86Op::FSub, biased, x, k, w, (I64)w);
		VReg hi = fresh(detail::kGp);
		cvti(hi, biased, w, pfx, 0x2c, true);
		alu(X86Op::Xor, hi, hi, gpConst((I64)0x8000000000000000ull)); // undo the bias

		VReg m = fresh(detail::kGp); // -(x >= 2^63)
		ucomis(m, x, k, w, CC_AE, false);
		blendU64(n, lo, hi, m);
	}

	void X86LowerPass::emitUIntToX87(ConvertNode* n, VReg s, U32 bits) {
		if(bits < 64) {
			VReg z = fresh(detail::kGp);
			mov(z, s);
			maskBits(z, bits);
			fild(x87SlotOf(n), z);
			return;
		}
		auto [hi, lo] = splitHalves(s);
		Slot fh = slot(reserve(16));
		fild(fh, hi);
		Slot flo = slot(reserve(16));
		fild(flo, lo);
		Slot k = slot(reserve(16));
		fldImm(k, 0x41f0000000000000ull); // 2^32
		x87Arith(X86Op::X87Mul, fh, fh, k);
		x87Arith(X86Op::X87Add, x87SlotOf(n), fh, flo); // exact in the 64-bit mantissa
	}

	void X86LowerPass::emitX87ToU64(ConvertNode* n, Slot x) {
		Slot k = slot(reserve(16));
		fldImm(k, 0x43e0000000000000ull); // 2^63

		VReg lo = fresh(detail::kGp); // while x < 2^63
		fistp(lo, x);

		Slot biased = slot(reserve(16));
		x87Arith(X86Op::X87Sub, biased, x, k);
		VReg hi = fresh(detail::kGp);
		fistp(hi, biased);
		alu(X86Op::Xor, hi, hi, gpConst((I64)0x8000000000000000ull)); // undo the bias

		VReg m = fresh(detail::kGp); // -(x >= 2^63)
		fucomi(m, x, k, CC_AE, false);
		blendU64(n, lo, hi, m);
	}

	void X86LowerPass::emitConvertX87(ConvertNode* n, Node* src, Opcode op) {
		const Type* st = src->getType();
		Slot x = slot(0);
		VReg s = kNoVReg;
		if(isX87Ty(st))
			x = x87Value(src);
		else if(isSseTy(st))
			s = sseValue(src);
		else
			s = gpValue(src);
		needScratch();
		switch(op) {
		case Opcode::FPExt:
			if(isX87Ty(st)) // long double -> long double: plain move
				fldSlot(x87SlotOf(n), x);
			else
				fldSse(x87SlotOf(n), s, opWidth(st));
			return;
		case Opcode::FPTrunc:
			fstpSse(vregFor(n), opWidth(n->getType()), x);
			return;
		case Opcode::SIToFP:
			fild(x87SlotOf(n), s);
			return;
		case Opcode::UIToFP:
			emitUIntToX87(n, s, intBits(st));
			return;
		case Opcode::FPToSI:
		case Opcode::FPToUI: {
			if(op == Opcode::FPToUI && intBits(n->getType()) >= 64) {
				emitX87ToU64(n, x);
				return;
			}
			VReg d = vregFor(n);
			fistp(d, x);
			signExtBits(d, intBits(n->getType()));
			return;
		}
		default:
			return;
		}
	}

} // namespace rat
