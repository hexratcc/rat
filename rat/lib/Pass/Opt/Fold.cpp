#include "Pass/Opt/Fold.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	U64 FoldPass::maskW(I64 v, U32 w) {
		if(w >= 64)
			return (U64)v;
		return (U64)v & (((U64)1 << w) - 1);
	}

	B32 FoldPass::wouldSignedDivOverflow(I64 a, I64 b) {
		return b == 0 || (a == INT64_MIN && b == -1);
	}

	I64 FoldPass::normalizeConst(I64 v, U32 w) {
		if(w == 1)
			return v & 1;
		return signExtend(v, w);
	}

	B32 FoldPass::isConstWithValue(Node* n, I64 want) {
		ConstantNode* c = dyn_cast<ConstantNode>(n);
		return c && c->getValue() == want;
	}

	B32 FoldPass::isZeroConst(Node* n) { return isConstWithValue(n, 0); }
	B32 FoldPass::isOneConst(Node* n) { return isConstWithValue(n, 1); }

	B32 FoldPass::isAllOnesConst(Node* n, U32 w) {
		ConstantNode* c = dyn_cast<ConstantNode>(n);
		return c && c->getValue() == normalizeConst(-1, w);
	}

	I32 FoldPass::pow2Log(Node* n, U32 w) {
		ConstantNode* c = dyn_cast<ConstantNode>(n);
		if(!c)
			return -1;
		U64 u = maskW(c->getValue(), w);
		if(u == 0 || (u & (u - 1)) != 0)
			return -1;
		I32 k = countTrailingZeros64(u);
		return (k >= 1 && k < (I32)w) ? k : -1;
	}

	B32 FoldPass::matchVarConst(Node* n, Opcode want, Node*& base, I64& c) {
		BinaryNode* b = dyn_cast<BinaryNode>(n);
		if(!b || b->getOpcode() != want)
			return false;
		if(ConstantNode* cr = dyn_cast<ConstantNode>(b->getRHS())) {
			base = b->getLHS();
			c = cr->getValue();
			return true;
		}
		if(want == Opcode::Add || want == Opcode::Mul)
			if(ConstantNode* cl = dyn_cast<ConstantNode>(b->getLHS())) {
				base = b->getRHS();
				c = cl->getValue();
				return true;
			}
		return false;
	}

	Node* constant(Function& fn, Type* type, I64 value) {
		// create a normalized constant
		return fn.create<ConstantNode>(type, FoldPass::normalizeConst(value, type->getIntWidth()));
	}

	Node* foldBinaryConst(Function& fn, Opcode op, Type* ty, U32 w, I64 a, I64 b) {
		auto k = [&](I64 v) { return constant(fn, ty, v); };
		switch(op) {
		case Opcode::Add:
			return k((I64)((U64)a + (U64)b));
		case Opcode::Sub:
			return k((I64)((U64)a - (U64)b));
		case Opcode::Mul:
			return k((I64)((U64)a * (U64)b));
		case Opcode::And:
			return k(a & b);
		case Opcode::Or:
			return k(a | b);
		case Opcode::Xor:
			return k(a ^ b);
		case Opcode::Shl:
			if(b >= 0 && b < (I64)w)
				return k((I64)((U64)a << b));
			return nullptr;
		case Opcode::LShr:
			if(b >= 0 && b < (I64)w)
				return k((I64)(FoldPass::maskW(a, w) >> b));
			return nullptr;
		case Opcode::AShr:
			if(b >= 0 && b < (I64)w)
				return k(signExtend(a, w) >> b);
			return nullptr;
		case Opcode::SDiv: {
			I64 sa = signExtend(a, w), sb = signExtend(b, w);
			if(FoldPass::wouldSignedDivOverflow(sa, sb))
				return nullptr;
			return k(sa / sb);
		}
		case Opcode::SRem: {
			I64 sa = signExtend(a, w), sb = signExtend(b, w);
			if(FoldPass::wouldSignedDivOverflow(sa, sb))
				return nullptr;
			return k(sa % sb);
		}
		case Opcode::UDiv: {
			U64 ua = FoldPass::maskW(a, w), ub = FoldPass::maskW(b, w);
			if(ub == 0)
				return nullptr;
			return k((I64)(ua / ub));
		}
		case Opcode::URem: {
			U64 ua = FoldPass::maskW(a, w), ub = FoldPass::maskW(b, w);
			if(ub == 0)
				return nullptr;
			return k((I64)(ua % ub));
		}
		default:
			return nullptr;
		}
	}

	Node* foldBinaryIdentity(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
		switch(op) {
		case Opcode::Add:
			if(FoldPass::isZeroConst(rhs))
				return lhs;
			if(FoldPass::isZeroConst(lhs))
				return rhs;
			break;
		case Opcode::Sub:
			if(FoldPass::isZeroConst(rhs))
				return lhs;
			if(lhs == rhs)
				return constant(fn, ty, 0);
			break;
		case Opcode::Mul:
			if(FoldPass::isOneConst(rhs))
				return lhs;
			if(FoldPass::isOneConst(lhs))
				return rhs;
			if(FoldPass::isZeroConst(rhs) || FoldPass::isZeroConst(lhs))
				return constant(fn, ty, 0);
			break;
		case Opcode::And:
			if(lhs == rhs)
				return lhs;
			if(FoldPass::isZeroConst(rhs) || FoldPass::isZeroConst(lhs))
				return constant(fn, ty, 0);
			if(FoldPass::isAllOnesConst(rhs, w))
				return lhs;
			if(FoldPass::isAllOnesConst(lhs, w))
				return rhs;
			break;
		case Opcode::Or:
			if(lhs == rhs)
				return lhs;
			if(FoldPass::isZeroConst(rhs))
				return lhs;
			if(FoldPass::isZeroConst(lhs))
				return rhs;
			if(FoldPass::isAllOnesConst(rhs, w) || FoldPass::isAllOnesConst(lhs, w))
				return constant(fn, ty, FoldPass::normalizeConst(-1, w));
			break;
		case Opcode::Xor:
			if(FoldPass::isZeroConst(rhs))
				return lhs;
			if(FoldPass::isZeroConst(lhs))
				return rhs;
			if(lhs == rhs)
				return constant(fn, ty, 0);
			break;
		case Opcode::Shl:
		case Opcode::LShr:
		case Opcode::AShr:
			if(FoldPass::isZeroConst(rhs))
				return lhs;
			break;
		default:
			break;
		}
		return nullptr;
	}

	Node* foldBinaryReassoc(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, ConstantNode* cr) {
		auto mkBin = [&](Opcode o, Node* x, I64 c) {
			return fn.create<BinaryNode>(o, ty, x, constant(fn, ty, c));
		};
		if((op == Opcode::Add || op == Opcode::Sub) && cr) {
			I64 c2 = cr->getValue();
			B32 outerNeg = op == Opcode::Sub;
			Node* base;
			I64 c1;
			for(Opcode inner : {Opcode::Add, Opcode::Sub}) {
				if(FoldPass::matchVarConst(lhs, inner, base, c1)) {
					B32 innerNeg = inner == Opcode::Sub;
					U64 t1 = innerNeg ? (U64)0 - (U64)c1 : (U64)c1;
					U64 t2 = outerNeg ? (U64)0 - (U64)c2 : (U64)c2;
					I64 k = FoldPass::normalizeConst((I64)(t1 + t2), w);
					if(k == 0)
						return base;
					return mkBin(Opcode::Add, base, k); // sub-by-const normalizes to add
				}
			}
		}
		if(op == Opcode::Mul && cr) {
			Node* base;
			I64 c1;
			if(FoldPass::matchVarConst(lhs, Opcode::Mul, base, c1)) {
				I64 k = FoldPass::normalizeConst((I64)((U64)c1 * (U64)cr->getValue()), w);
				if(k == 0)
					return constant(fn, ty, 0);
				if(k == 1)
					return base;
				return mkBin(Opcode::Mul, base, k);
			}
		}
		return nullptr;
	}

	// hacker's delight 10-9: magic number for unsigned 32-bit division by d
	struct MagicU32 {
		U32 m;
		U32 s;
		B32 a; // "add" indicator: the 33-bit constant case
	};

	MagicU32 magicU32(U32 d) {
		MagicU32 mag = {0, 0, false};
		U32 nc = (U32)-1 - (U32)(-(I64)d) % d;
		U32 p = 31;
		U32 q1 = 0x80000000u / nc, r1 = 0x80000000u - q1 * nc;
		U32 q2 = 0x7FFFFFFFu / d, r2 = 0x7FFFFFFFu - q2 * d;
		U32 delta;
		do {
			p = p + 1;
			if(r1 >= nc - r1) {
				q1 = 2 * q1 + 1;
				r1 = 2 * r1 - nc;
			} else {
				q1 = 2 * q1;
				r1 = 2 * r1;
			}
			if(r2 + 1 >= d - r2) {
				if(q2 >= 0x7FFFFFFFu)
					mag.a = true;
				q2 = 2 * q2 + 1;
				r2 = 2 * r2 + 1 - d;
			} else {
				if(q2 >= 0x80000000u)
					mag.a = true;
				q2 = 2 * q2;
				r2 = 2 * r2 + 1;
			}
			delta = d - 1 - r2;
		} while(p < 64 && (q1 < delta || (q1 == delta && r1 == 0)));
		mag.m = q2 + 1;
		mag.s = p - 32;
		return mag;
	}

	// gacker's delight 10-4: magic number for signed 32-bit division by d
	struct MagicS32 {
		I32 m;
		U32 s;
	};

	MagicS32 magicS32(I32 d) {
		const U32 two31 = 0x80000000u;
		U32 ad = (U32)(d < 0 ? -(I64)d : d);
		U32 t = two31 + ((U32)d >> 31);
		U32 anc = t - 1 - t % ad;
		U32 p = 31;
		U32 q1 = two31 / anc, r1 = two31 - q1 * anc;
		U32 q2 = two31 / ad, r2 = two31 - q2 * ad;
		U32 delta;
		do {
			p = p + 1;
			q1 = 2 * q1;
			r1 = 2 * r1;
			if(r1 >= anc) {
				q1 = q1 + 1;
				r1 = r1 - anc;
			}
			q2 = 2 * q2;
			r2 = 2 * r2;
			if(r2 >= ad) {
				q2 = q2 + 1;
				r2 = r2 - ad;
			}
			delta = ad - r2;
		} while(q1 < delta || (q1 == delta && r1 == 0));
		MagicS32 mag;
		mag.m = (I32)(q2 + 1);
		if(d < 0)
			mag.m = -mag.m;
		mag.s = p - 32;
		return mag;
	}

	// x udiv d as a multiply-shift over a 64-bit widening (w == 32, d not a
	// power of two, d >= 3)
	Node* buildUDivByConst(Function& fn, Type* ty, Node* x, U32 d) {
		Type* i64 = fn.types().getInt(64);
		auto k64 = [&](I64 v) { return constant(fn, i64, v); };
		auto bin = [&](Opcode o, Node* a, Node* b) { return fn.create<BinaryNode>(o, i64, a, b); };
		MagicU32 mg = magicU32(d);
		Node* x64 = fn.create<ConvertNode>(Opcode::ZExt, i64, x);
		Node* prod = bin(Opcode::Mul, x64, k64((I64)(U64)mg.m));
		Node* q64;
		if(!mg.a) {
			q64 = bin(Opcode::LShr, prod, k64(32 + (I64)mg.s));
		} else { // 33-bit constant: t = hi32(prod); q = (((x - t) >> 1) + t) >> (s - 1)
			Node* t = bin(Opcode::LShr, prod, k64(32));
			Node* half = bin(Opcode::LShr, bin(Opcode::Sub, x64, t), k64(1));
			q64 = bin(Opcode::LShr, bin(Opcode::Add, half, t), k64((I64)mg.s - 1));
		}
		return fn.create<ConvertNode>(Opcode::Trunc, ty, q64);
	}

	// x sdiv d as a multiply-shift over a 64-bit widening (w == 32, |d| >= 2,
	// d != INT32_MIN)
	Node* buildSDivByConst(Function& fn, Type* ty, Node* x, I32 d) {
		Type* i64 = fn.types().getInt(64);
		auto k64 = [&](I64 v) { return constant(fn, i64, v); };
		auto bin = [&](Opcode o, Node* a, Node* b) { return fn.create<BinaryNode>(o, i64, a, b); };
		MagicS32 mg = magicS32(d);
		Node* x64 = fn.create<ConvertNode>(Opcode::SExt, i64, x);
		Node* q0 = bin(Opcode::AShr, bin(Opcode::Mul, x64, k64(mg.m)), k64(32));
		if(d > 0 && mg.m < 0)
			q0 = bin(Opcode::Add, q0, x64);
		else if(d < 0 && mg.m > 0)
			q0 = bin(Opcode::Sub, q0, x64);
		Node* q = bin(Opcode::AShr, q0, k64((I64)mg.s));
		q = bin(Opcode::Add, q, bin(Opcode::LShr, q, k64(63))); // add 1 if the quotient is negative
		return fn.create<ConvertNode>(Opcode::Trunc, ty, q);
	}

	// x sdiv 2^k via bias-and-shift (w == 32, k in [1, 30])
	Node* buildSDivByPow2(Function& fn, Type* ty, Node* x, I32 k) {
		auto ki = [&](I64 v) { return constant(fn, ty, v); };
		auto bin = [&](Opcode o, Node* a, Node* b) { return fn.create<BinaryNode>(o, ty, a, b); };
		Node* sign = bin(Opcode::AShr, x, ki(31));
		Node* bias = bin(Opcode::LShr, sign, ki(32 - k));
		return bin(Opcode::AShr, bin(Opcode::Add, x, bias), ki(k));
	}

	// quotient expansion for any supported constant divisor, or nullptr
	Node* buildDivByConst(Function& fn, Opcode op, Type* ty, U32 w, Node* x, ConstantNode* c) {
		if(w != 32)
			return nullptr;
		if(op == Opcode::UDiv || op == Opcode::URem) {
			U32 d = (U32)FoldPass::maskW(c->getValue(), w);
			if(d < 3 || (d & (d - 1)) == 0)
				return nullptr; // 0/1/2 and powers of two are handled elsewhere
			return buildUDivByConst(fn, ty, x, d);
		}
		I32 d = (I32)signExtend(c->getValue(), w);
		if(d == 0 || d == 1 || d == -1 || d == INT32_MIN)
			return nullptr;
		if(d > 0 && (d & (d - 1)) == 0)
			return buildSDivByPow2(fn, ty, x, countTrailingZeros64((U32)d));
		return buildSDivByConst(fn, ty, x, d);
	}

	Node* foldBinaryStrength(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
		auto mkBin = [&](Opcode o, Node* x, I64 c) {
			return fn.create<BinaryNode>(o, ty, x, constant(fn, ty, c));
		};
		switch(op) {
		case Opcode::Mul:
			if(I32 k = FoldPass::pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::Shl, lhs, k);
			break;
		case Opcode::UDiv:
			if(I32 k = FoldPass::pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::LShr, lhs, k);
			if(ConstantNode* c = dyn_cast<ConstantNode>(rhs))
				return buildDivByConst(fn, op, ty, w, lhs, c);
			break;
		case Opcode::URem:
			if(I32 k = FoldPass::pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::And, lhs, (I64)((1ULL << k) - 1));
			if(ConstantNode* c = dyn_cast<ConstantNode>(rhs))
				if(Node* q = buildDivByConst(fn, op, ty, w, lhs, c)) // r = x - (x / d) * d
					return fn.create<BinaryNode>(
							Opcode::Sub,
							ty,
							lhs,
							fn.create<BinaryNode>(Opcode::Mul, ty, q, constant(fn, ty, c->getValue())));
			break;
		case Opcode::SDiv:
			if(ConstantNode* c = dyn_cast<ConstantNode>(rhs))
				return buildDivByConst(fn, op, ty, w, lhs, c);
			break;
		case Opcode::SRem:
			if(ConstantNode* c = dyn_cast<ConstantNode>(rhs))
				if(Node* q = buildDivByConst(fn, op, ty, w, lhs, c)) // r = x - (x / d) * d
					return fn.create<BinaryNode>(
							Opcode::Sub,
							ty,
							lhs,
							fn.create<BinaryNode>(Opcode::Mul, ty, q, constant(fn, ty, c->getValue())));
			break;
		default:
			break;
		}
		return nullptr;
	}

	Node* foldShiftOfShift(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
		if(op != Opcode::Shl && op != Opcode::LShr && op != Opcode::AShr)
			return nullptr;
		auto mkBin = [&](Opcode o, Node* x, I64 c) {
			return fn.create<BinaryNode>(o, ty, x, constant(fn, ty, c));
		};
		BinaryNode* in = dyn_cast<BinaryNode>(lhs);
		ConstantNode* cb = dyn_cast<ConstantNode>(rhs);
		if(in && in->getOpcode() == op && cb) {
			ConstantNode* ca = dyn_cast<ConstantNode>(in->getRHS());
			I64 a = ca ? ca->getValue() : -1;
			I64 b = cb->getValue();
			if(ca && a >= 0 && a < (I64)w && b >= 0 && b < (I64)w) {
				Node* x = in->getLHS();
				I64 sum = a + b;
				if(sum >= (I64)w)
					return op == Opcode::AShr ? mkBin(Opcode::AShr, x, w - 1) : constant(fn, ty, 0);
				return mkBin(op, x, sum);
			}
		}
		return nullptr;
	}

	Node* foldBinary(Function& fn, Opcode op, Node* lhs, Node* rhs) {
		Type* ty = lhs->getType();
		if(!ty->isInt())
			return nullptr;
		U32 w = ty->getIntWidth();

		ConstantNode* cl = dyn_cast<ConstantNode>(lhs);
		ConstantNode* cr = dyn_cast<ConstantNode>(rhs);

		if(cl && cr)
			return foldBinaryConst(fn, op, ty, w, cl->getValue(), cr->getValue());

		// normalize a lone constant to the RHS for commutative ops, so every rule
		// below only has to look on one side
		if(cl && !cr &&
			 (op == Opcode::Add || op == Opcode::Mul || op == Opcode::And || op == Opcode::Or ||
				op == Opcode::Xor)) {
			std::swap(lhs, rhs);
			std::swap(cl, cr);
		}

		if(Node* r = foldBinaryIdentity(fn, op, ty, w, lhs, rhs))
			return r;
		if(Node* r = foldBinaryReassoc(fn, op, ty, w, lhs, cr))
			return r;
		if(Node* r = foldBinaryStrength(fn, op, ty, w, lhs, rhs))
			return r;
		if(Node* r = foldShiftOfShift(fn, op, ty, w, lhs, rhs))
			return r;
		return nullptr;
	}

	Node* foldUnary(Function& fn, Opcode op, Node* operand) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(operand)) {
			if(!operand->getType()->isInt())
				return nullptr;
			U32 w = operand->getType()->getIntWidth();
			I64 x = c->getValue();
			I64 r = (op == Opcode::Neg) ? -x : ~x;
			return constant(fn, operand->getType(), FoldPass::normalizeConst(r, w));
		}
		return nullptr;
	}

	Node* foldCompare(Function& fn, Opcode op, Node* lhs, Node* rhs) {
		Type* ty = lhs->getType();
		if(!ty->isInt())
			return nullptr;
		U32 w = ty->getIntWidth();

		ConstantNode* cl = dyn_cast<ConstantNode>(lhs);
		ConstantNode* cr = dyn_cast<ConstantNode>(rhs);
		if(cl && cr) {
			I64 a = cl->getValue(), b = cr->getValue();
			B32 res = false;
			switch(op) {
			case Opcode::Eq:
				res = FoldPass::maskW(a, w) == FoldPass::maskW(b, w);
				break;
			case Opcode::Ne:
				res = FoldPass::maskW(a, w) != FoldPass::maskW(b, w);
				break;
			case Opcode::Slt:
				res = signExtend(a, w) < signExtend(b, w);
				break;
			case Opcode::Sle:
				res = signExtend(a, w) <= signExtend(b, w);
				break;
			case Opcode::Ult:
				res = FoldPass::maskW(a, w) < FoldPass::maskW(b, w);
				break;
			case Opcode::Ule:
				res = FoldPass::maskW(a, w) <= FoldPass::maskW(b, w);
				break;
			default:
				return nullptr;
			}
			return fn.constBool(res);
		}

		if(lhs == rhs) {
			switch(op) {
			case Opcode::Eq:
			case Opcode::Sle:
			case Opcode::Ule:
				return fn.constBool(true);
			case Opcode::Ne:
			case Opcode::Slt:
			case Opcode::Ult:
				return fn.constBool(false);
			default:
				return nullptr;
			}
		}

		// collapse boolean re-tests: comparing an i1 (possibly zero-extended)
		// against 0/1 yields the i1 itself or its complement
		if((op == Opcode::Eq || op == Opcode::Ne) && cr &&
			 (cr->getValue() == 0 || cr->getValue() == 1)) {
			Node* boolean = nullptr;
			if(ty->getIntWidth() == 1) {
				boolean = lhs;
			} else if(ConvertNode* cv = dyn_cast<ConvertNode>(lhs)) {
				if(cv->getOpcode() == Opcode::ZExt && cv->getOperand()->getType()->isInt() &&
					 cv->getOperand()->getType()->getIntWidth() == 1)
					boolean = cv->getOperand();
			}
			if(boolean) {
				// (b != 0) == b, (b == 1) == b; the other two are the complement
				B32 direct = (op == Opcode::Ne) == (cr->getValue() == 0);
				if(direct)
					return boolean;
				Type* i1 = boolean->getType();
				return fn.create<BinaryNode>(Opcode::Xor, i1, boolean, constant(fn, i1, 1));
			}
		}
		return nullptr;
	}

	Node* foldConvert(Function& fn, Opcode op, Node* operand, Type* destType) {
		if(operand->getType() == destType)
			return operand;
		ConstantNode* c = dyn_cast<ConstantNode>(operand);
		if(!c || !operand->getType()->isInt() || !destType->isInt())
			return nullptr;
		U32 srcW = operand->getType()->getIntWidth();
		U32 dstW = destType->getIntWidth();
		I64 x = c->getValue();
		I64 res = 0;
		switch(op) {
		case Opcode::Trunc:
			res = FoldPass::normalizeConst(x, dstW);
			break;
		case Opcode::SExt:
			res = FoldPass::normalizeConst(signExtend(x, srcW), dstW);
			break;
		case Opcode::ZExt:
			res = FoldPass::normalizeConst((I64)FoldPass::maskW(x, srcW), dstW);
			break;
		default:
			return nullptr;
		}
		return constant(fn, destType, res);
	}

	Node* simplify(Function& fn, Node* n) {
		Opcode op = n->getOpcode();
		if(isBinaryOpcode(op)) {
			BinaryNode* b = cast<BinaryNode>(n);
			if(Node* r = foldBinary(fn, op, b->getLHS(), b->getRHS()))
				return r;
		} else if(isUnaryOpcode(op)) {
			UnaryNode* u = cast<UnaryNode>(n);
			if(Node* r = foldUnary(fn, op, u->getOperand()))
				return r;
		} else if(isCompareOpcode(op)) {
			CompareNode* c = cast<CompareNode>(n);
			if(Node* r = foldCompare(fn, op, c->getLHS(), c->getRHS()))
				return r;
		} else if(isConvertOpcode(op)) {
			ConvertNode* c = cast<ConvertNode>(n);
			if(Node* r = foldConvert(fn, op, c->getOperand(), n->getType()))
				return r;
		}
		return n;
	}

	U32 FoldPass::runOnFunction(Function& fn, const TargetInfo&) {
		U32 changed = 0;
		B32 again = true;
		while(again) {
			again = false;
			List<Node*> work;
			for(Node* n : fn)
				if(isArithmeticOpcode(n->getOpcode()))
					work.push_back(n);
			for(Node* n : work) {
				if(!n->hasUsers())
					continue;
				Node* s = simplify(fn, n);
				if(s != n) {
					n->replaceAllUsesWith(s);
					++changed;
					again = true;
				}
			}
		}
		if(changed)
			fn.eliminateDeadNodes();
		return changed;
	}

	const C8* FoldPass::name() const { return "fold"; }
} // namespace rat
