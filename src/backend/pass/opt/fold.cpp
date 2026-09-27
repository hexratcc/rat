#include "pass/opt/fold.h"

#include "ir/function.h"
#include "ir/node.h"
#include "ir/type.h"

namespace rat {
	namespace detail {
		U64 maskW(I64 v, U32 w) {
			if(w >= 64)
				return (U64)v;
			return (U64)v & (((U64)1 << w) - 1);
		}

		I64 normalizeConst(I64 v, U32 w) {
			if(w == 1)
				return v & 1;
			return signExtend(v, w);
		}

		B32 isConstWithValue(Node* n, I64 want) {
			ConstantNode* c = dyn_cast<ConstantNode>(n);
			return c && c->getValue() == want;
		}

		B32 isZeroConst(Node* n) { return isConstWithValue(n, 0); }
		B32 isAllOnesConst(Node* n, U32 w) { return isConstWithValue(n, normalizeConst(-1, w)); }

		I32 pow2Log(Node* n, U32 w) {
			ConstantNode* c = dyn_cast<ConstantNode>(n);
			if(!c)
				return -1;
			U64 u = maskW(c->getValue(), w);
			if(u == 0 || (u & (u - 1)) != 0)
				return -1;
			I32 k = countTrailingZeros64(u);
			return (k >= 1 && k < (I32)w) ? k : -1;
		}

		B32 matchVarConst(Node* n, Opcode want, Node*& base, I64& c) {
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

		Node* mkBin(Function& fn, Opcode op, Type* ty, Node* x, I64 c) {
			return fn.create<BinaryNode>(op, ty, x, constant(fn, ty, c));
		}
	} // namespace detail

	Node* constant(Function& fn, Type* type, I64 value) {
		// create a normalized constant
		return fn.create<ConstantNode>(type, detail::normalizeConst(value, type->getIntWidth()));
	}

	B32 evalBinaryConst(Opcode op, U32 w, I64 a, I64 b, I64& out) {
		I64 r;
		switch(op) {
		case Opcode::Add:
			r = (I64)((U64)a + (U64)b);
			break;
		case Opcode::Sub:
			r = (I64)((U64)a - (U64)b);
			break;
		case Opcode::Mul:
			r = (I64)((U64)a * (U64)b);
			break;
		case Opcode::And:
			r = a & b;
			break;
		case Opcode::Or:
			r = a | b;
			break;
		case Opcode::Xor:
			r = a ^ b;
			break;
		case Opcode::Shl:
		case Opcode::LShr:
		case Opcode::AShr:
			if(b < 0 || b >= (I64)w)
				return false;
			if(op == Opcode::Shl)
				r = (I64)((U64)a << b);
			else if(op == Opcode::LShr)
				r = (I64)(detail::maskW(a, w) >> b);
			else
				r = signExtend(a, w) >> b;
			break;
		case Opcode::Rotl:
		case Opcode::Rotr: {
			if(b < 0)
				return false;
			U64 ua = detail::maskW(a, w);
			U32 c = (U32)((U64)b & (w - 1));
			if(op == Opcode::Rotr)
				c = c ? w - c : 0;
			r = (I64)(c ? ((ua << c) | (ua >> (w - c))) : ua);
			break;
		}
		case Opcode::SDiv:
		case Opcode::SRem: {
			I64 sa = signExtend(a, w), sb = signExtend(b, w);
			if(sb == 0 || (sa == INT64_MIN && sb == -1)) // div by 0 or INT_MIN-1
				return false;
			r = op == Opcode::SDiv ? sa / sb : sa % sb;
			break;
		}
		case Opcode::UDiv:
		case Opcode::URem: {
			U64 ua = detail::maskW(a, w), ub = detail::maskW(b, w);
			if(ub == 0)
				return false;
			r = (I64)(op == Opcode::UDiv ? ua / ub : ua % ub);
			break;
		}
		default:
			return false;
		}
		out = detail::normalizeConst(r, w);
		return true;
	}

	B32 evalUnaryConst(Opcode op, U32 w, I64 x, I64& out) {
		U64 v = detail::maskW(x, w);
		U32 n = 0;
		switch(op) {
		case Opcode::Neg:
		case Opcode::Not:
			out = detail::normalizeConst(op == Opcode::Neg ? -x : ~x, w);
			return true;
		case Opcode::Clz:
		case Opcode::Ctz:
			// undefined at zero
			if(v == 0)
				return false;
			if(op == Opcode::Clz)
				while(((v >> (w - 1 - n)) & 1) == 0)
					++n;
			else
				n = (U32)countTrailingZeros64(v);
			out = (I64)n;
			return true;
		case Opcode::Popcnt:
			for(; v != 0; v &= v - 1)
				++n;
			out = (I64)n;
			return true;
		case Opcode::Bswap: {
			U64 r = 0;
			for(n = 0; n < w; n += 8)
				r = (r << 8) | ((v >> n) & 0xff);
			out = detail::normalizeConst((I64)r, w);
			return true;
		}
		default:
			return false;
		}
	}

	B32 evalCompareConst(Opcode op, U32 w, I64 a, I64 b, I64& out) {
		U64 ua = detail::maskW(a, w), ub = detail::maskW(b, w);
		I64 sa = signExtend(a, w), sb = signExtend(b, w);
		switch(op) {
		case Opcode::Eq:
			out = ua == ub;
			return true;
		case Opcode::Ne:
			out = ua != ub;
			return true;
		case Opcode::Slt:
			out = sa < sb;
			return true;
		case Opcode::Sle:
			out = sa <= sb;
			return true;
		case Opcode::Ult:
			out = ua < ub;
			return true;
		case Opcode::Ule:
			out = ua <= ub;
			return true;
		default:
			return false;
		}
	}

	B32 evalConvertConst(Opcode op, U32 srcW, U32 dstW, I64 x, I64& out) {
		switch(op) {
		case Opcode::Trunc:
			out = detail::normalizeConst(x, dstW);
			return true;
		case Opcode::SExt:
			out = detail::normalizeConst(signExtend(x, srcW), dstW);
			return true;
		case Opcode::ZExt:
			out = detail::normalizeConst((I64)detail::maskW(x, srcW), dstW);
			return true;
		default:
			return false;
		}
	}

	namespace detail {
		Node* foldBinaryIdentity(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
			switch(op) {
			case Opcode::Add:
				if(isZeroConst(rhs))
					return lhs;
				break;
			case Opcode::Sub:
				if(isZeroConst(rhs))
					return lhs;
				if(lhs == rhs)
					return constant(fn, ty, 0);
				break;
			case Opcode::Mul:
				if(isConstWithValue(rhs, 1))
					return lhs;
				if(isZeroConst(rhs))
					return constant(fn, ty, 0);
				break;
			case Opcode::And:
				if(lhs == rhs)
					return lhs;
				if(isZeroConst(rhs))
					return constant(fn, ty, 0);
				if(isAllOnesConst(rhs, w))
					return lhs;
				return foldSExtMask(fn, ty, lhs, rhs);
			case Opcode::Or:
				if(lhs == rhs)
					return lhs;
				if(isZeroConst(rhs))
					return lhs;
				if(isAllOnesConst(rhs, w))
					return constant(fn, ty, -1);
				return foldRotate(fn, ty, w, lhs, rhs);
			case Opcode::Xor:
				if(isZeroConst(rhs))
					return lhs;
				if(lhs == rhs)
					return constant(fn, ty, 0);
				break;
			case Opcode::Shl:
			case Opcode::LShr:
			case Opcode::AShr:
				if(isZeroConst(rhs))
					return lhs;
				break;
			default:
				break;
			}
			return nullptr;
		}

		// sext(x) & (2^sw-1) == zext(x)
		Node* foldSExtMask(Function& fn, Type* ty, Node* lhs, Node* rhs) {
			ConvertNode* cv = dyn_cast<ConvertNode>(lhs);
			if(!cv || cv->getOpcode() != Opcode::SExt)
				return nullptr;
			Type* srcTy = cv->getOperand()->getType();
			if(!srcTy || !srcTy->isInt() || srcTy->getIntWidth() >= 64)
				return nullptr;
			ConstantNode* mc = dyn_cast<ConstantNode>(rhs);
			if(!mc || (U64)mc->getValue() != ((1ULL << srcTy->getIntWidth()) - 1))
				return nullptr;
			return fn.create<ConvertNode>(Opcode::ZExt, ty, cv->getOperand());
		}

		// (x<<c)|(x>>(w-c)) == rotate (32/64-bit only, one rol/ror)
		Node* foldRotate(Function& fn, Type* ty, U32 w, Node* lhs, Node* rhs) {
			if(w != 32 && w != 64)
				return nullptr;
			BinaryNode* bl = dyn_cast<BinaryNode>(lhs);
			BinaryNode* br = dyn_cast<BinaryNode>(rhs);
			if(!bl || !br)
				return nullptr;
			if(bl->getOpcode() == Opcode::LShr && br->getOpcode() == Opcode::Shl)
				std::swap(bl, br);
			if(bl->getOpcode() != Opcode::Shl || br->getOpcode() != Opcode::LShr ||
				 bl->getLHS() != br->getLHS())
				return nullptr;
			ConstantNode* cl = dyn_cast<ConstantNode>(bl->getRHS());
			ConstantNode* cr = dyn_cast<ConstantNode>(br->getRHS());
			if(!cl || !cr || cl->getValue() <= 0 || cr->getValue() <= 0 ||
				 cl->getValue() + cr->getValue() != (I64)w)
				return nullptr;
			return fn.create<BinaryNode>(Opcode::Rotr, ty, br->getLHS(), br->getRHS());
		}

		Node* foldBinaryReassoc(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, ConstantNode* cr) {
			if(!cr)
				return nullptr;
			Node* base;
			I64 c1;
			if(op == Opcode::Add || op == Opcode::Sub) {
				I64 c2 = cr->getValue();
				B32 outerNeg = op == Opcode::Sub;
				for(Opcode inner : {Opcode::Add, Opcode::Sub}) {
					if(!matchVarConst(lhs, inner, base, c1))
						continue;
					B32 innerNeg = inner == Opcode::Sub;
					U64 t1 = innerNeg ? (U64)0 - (U64)c1 : (U64)c1;
					U64 t2 = outerNeg ? (U64)0 - (U64)c2 : (U64)c2;
					I64 k = normalizeConst((I64)(t1 + t2), w);
					if(k == 0)
						return base;
					return mkBin(fn, Opcode::Add, ty, base, k); // sub-by-const normalizes to add
				}
			}
			if(op == Opcode::Mul && matchVarConst(lhs, Opcode::Mul, base, c1)) {
				I64 k = normalizeConst((I64)((U64)c1 * (U64)cr->getValue()), w);
				if(k == 0)
					return constant(fn, ty, 0);
				if(k == 1)
					return base;
				return mkBin(fn, Opcode::Mul, ty, base, k);
			}
			return nullptr;
		}

		// hacker's delight 10-9
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

		// hacker's delight 10-4
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
			MagicU32 mg = magicU32(d);
			Node* x64 = fn.create<ConvertNode>(Opcode::ZExt, i64, x);
			Node* prod = mkBin(fn, Opcode::Mul, i64, x64, (I64)(U64)mg.m);
			Node* q64;
			if(!mg.a) {
				q64 = mkBin(fn, Opcode::LShr, i64, prod, 32 + (I64)mg.s);
			} else { // 33-bit constant: t = hi32(prod); q = (((x - t) >> 1) + t) >> (s - 1)
				Node* t = mkBin(fn, Opcode::LShr, i64, prod, 32);
				Node* one = constant(fn, i64, 1);
				Node* diff = fn.create<BinaryNode>(Opcode::Sub, i64, x64, t);
				Node* half = fn.create<BinaryNode>(Opcode::LShr, i64, diff, one);
				Node* shift = constant(fn, i64, (I64)mg.s - 1);
				Node* sum = fn.create<BinaryNode>(Opcode::Add, i64, half, t);
				q64 = fn.create<BinaryNode>(Opcode::LShr, i64, sum, shift);
			}
			return fn.create<ConvertNode>(Opcode::Trunc, ty, q64);
		}

		// x sdiv d as a multiply-shift over a 64-bit widening (w == 32, |d| >= 2,
		// d != INT32_MIN)
		Node* buildSDivByConst(Function& fn, Type* ty, Node* x, I32 d) {
			Type* i64 = fn.types().getInt(64);
			MagicS32 mg = magicS32(d);
			Node* x64 = fn.create<ConvertNode>(Opcode::SExt, i64, x);
			Node* hi = constant(fn, i64, 32);
			Node* prod = mkBin(fn, Opcode::Mul, i64, x64, mg.m);
			Node* q0 = fn.create<BinaryNode>(Opcode::AShr, i64, prod, hi);
			if(d > 0 && mg.m < 0)
				q0 = fn.create<BinaryNode>(Opcode::Add, i64, q0, x64);
			else if(d < 0 && mg.m > 0)
				q0 = fn.create<BinaryNode>(Opcode::Sub, i64, q0, x64);
			Node* q = mkBin(fn, Opcode::AShr, i64, q0, (I64)mg.s);
			Node* sign = mkBin(fn, Opcode::LShr, i64, q, 63);
			q = fn.create<BinaryNode>(Opcode::Add, i64, q, sign); // add 1 if the quotient is negative
			return fn.create<ConvertNode>(Opcode::Trunc, ty, q);
		}

		// x sdiv 2^k via bias-and-shift, any width, k in [1, w-2]
		Node* buildSDivByPow2(Function& fn, Type* ty, Node* x, U32 w, I32 k) {
			Node* sign = mkBin(fn, Opcode::AShr, ty, x, (I64)w - 1);
			Node* bias = mkBin(fn, Opcode::LShr, ty, sign, (I64)w - k);
			Node* shift = constant(fn, ty, k);
			Node* biased = fn.create<BinaryNode>(Opcode::Add, ty, x, bias);
			return fn.create<BinaryNode>(Opcode::AShr, ty, biased, shift);
		}

		// quotient expansion for any supported constant divisor, or nullptr
		Node* buildDivByConst(Function& fn, Opcode op, Type* ty, U32 w, Node* x, ConstantNode* c) {
			if(op == Opcode::UDiv || op == Opcode::URem) {
				if(w != 32)
					return nullptr; // magic-number path is 32-bit only
				U32 d = (U32)maskW(c->getValue(), w);
				if(d < 3 || (d & (d - 1)) == 0)
					return nullptr; // 0/1/2 and powers of two are handled elsewhere
				return buildUDivByConst(fn, ty, x, d);
			}
			if(w < 2 || w > 64)
				return nullptr;
			I64 d = signExtend(c->getValue(), w);
			I64 mostNegative = -((I64)1 << (w - 1));
			if(d == 0 || d == 1 || d == -1 || d == mostNegative)
				return nullptr;
			if(d > 0 && (d & (d - 1)) == 0)
				return buildSDivByPow2(fn, ty, x, w, countTrailingZeros64((U64)d));
			if(w != 32)
				return nullptr;
			return buildSDivByConst(fn, ty, x, (I32)d);
		}

		Node* foldBinaryStrength(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
			switch(op) {
			case Opcode::Mul:
				if(I32 k = pow2Log(rhs, w); k > 0)
					return mkBin(fn, Opcode::Shl, ty, lhs, k);
				break;
			case Opcode::UDiv:
				if(I32 k = pow2Log(rhs, w); k > 0)
					return mkBin(fn, Opcode::LShr, ty, lhs, k);
				[[fallthrough]];
			case Opcode::SDiv:
				if(ConstantNode* c = dyn_cast<ConstantNode>(rhs))
					return buildDivByConst(fn, op, ty, w, lhs, c);
				break;
			case Opcode::URem:
				if(I32 k = pow2Log(rhs, w); k > 0)
					return mkBin(fn, Opcode::And, ty, lhs, (I64)((1ULL << k) - 1));
				[[fallthrough]];
			case Opcode::SRem:
				if(ConstantNode* c = dyn_cast<ConstantNode>(rhs))
					if(Node* q = buildDivByConst(fn, op, ty, w, lhs, c)) // r = x - (x / d) * d
						return fn.create<BinaryNode>(
								Opcode::Sub, ty, lhs, mkBin(fn, Opcode::Mul, ty, q, c->getValue()));
				break;
			default:
				break;
			}
			return nullptr;
		}

		Node* foldShiftOfShift(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
			if(op != Opcode::Shl && op != Opcode::LShr && op != Opcode::AShr)
				return nullptr;
			BinaryNode* in = dyn_cast<BinaryNode>(lhs);
			ConstantNode* cb = dyn_cast<ConstantNode>(rhs);
			if(!in || in->getOpcode() != op || !cb)
				return nullptr;
			ConstantNode* ca = dyn_cast<ConstantNode>(in->getRHS());
			if(!ca)
				return nullptr;
			I64 a = ca->getValue(), b = cb->getValue();
			if(a < 0 || a >= (I64)w || b < 0 || b >= (I64)w)
				return nullptr;
			Node* x = in->getLHS();
			if(a + b < (I64)w)
				return mkBin(fn, op, ty, x, a + b);
			if(op == Opcode::AShr)
				return mkBin(fn, Opcode::AShr, ty, x, w - 1);
			return constant(fn, ty, 0);
		}

		Node* foldBinary(Function& fn, Opcode op, Node* lhs, Node* rhs) {
			Type* ty = lhs->getType();
			if(ty->isPtr()) {
				// ptr arithmetic never reaches the int rules; peel the zero-offset identity
				if((op == Opcode::Add || op == Opcode::Sub) && isZeroConst(rhs))
					return lhs;
				return nullptr;
			}
			if(!ty->isInt())
				return nullptr;
			U32 w = ty->getIntWidth();

			ConstantNode* cl = dyn_cast<ConstantNode>(lhs);
			ConstantNode* cr = dyn_cast<ConstantNode>(rhs);

			if(cl && cr) {
				I64 res;
				if(!evalBinaryConst(op, w, cl->getValue(), cr->getValue(), res))
					return nullptr;
				return constant(fn, ty, res);
			}

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
			return foldShiftOfShift(fn, op, ty, w, lhs, rhs);
		}

		Node* foldUnary(Function& fn, Opcode op, Node* operand) {
			ConstantNode* c = dyn_cast<ConstantNode>(operand);
			if(!c || !operand->getType()->isInt())
				return nullptr;
			I64 r;
			if(!evalUnaryConst(op, operand->getType()->getIntWidth(), c->getValue(), r))
				return nullptr;
			return constant(fn, operand->getType(), r);
		}

		Node* foldCompare(Function& fn, Opcode op, Node* lhs, Node* rhs) {
			Type* ty = lhs->getType();
			if(!ty->isInt())
				return nullptr;

			ConstantNode* cl = dyn_cast<ConstantNode>(lhs);
			ConstantNode* cr = dyn_cast<ConstantNode>(rhs);
			if(cl && cr) {
				I64 res;
				if(!evalCompareConst(op, ty->getIntWidth(), cl->getValue(), cr->getValue(), res))
					return nullptr;
				return fn.constBool(res != 0);
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
			return foldBoolRetest(fn, op, lhs, cr);
		}

		// collapse boolean re-tests: comparing an i1 (possibly zero-extended)
		// against 0/1 yields the i1 itself or its complement
		Node* foldBoolRetest(Function& fn, Opcode op, Node* lhs, ConstantNode* cr) {
			if((op != Opcode::Eq && op != Opcode::Ne) || !cr ||
				 (cr->getValue() != 0 && cr->getValue() != 1))
				return nullptr;
			Node* boolean = nullptr;
			if(lhs->getType()->getIntWidth() == 1) {
				boolean = lhs;
			} else if(ConvertNode* cv = dyn_cast<ConvertNode>(lhs)) {
				if(cv->getOpcode() == Opcode::ZExt && cv->getOperand()->getType()->isInt() &&
					 cv->getOperand()->getType()->getIntWidth() == 1)
					boolean = cv->getOperand();
			}
			if(!boolean)
				return nullptr;
			// (b != 0) == b, (b == 1) == b; the other two are the complement
			B32 direct = (op == Opcode::Ne) == (cr->getValue() == 0);
			if(direct)
				return boolean;
			Type* i1 = boolean->getType();
			return fn.create<BinaryNode>(Opcode::Xor, i1, boolean, constant(fn, i1, 1));
		}

		Node* foldConvert(Function& fn, Opcode op, Node* operand, Type* destType) {
			if(operand->getType() == destType)
				return operand;
			ConstantNode* c = dyn_cast<ConstantNode>(operand);
			if(!c || !operand->getType()->isInt() || !destType->isInt())
				return nullptr;
			U32 srcW = operand->getType()->getIntWidth();
			U32 dstW = destType->getIntWidth();
			I64 r;
			if(!evalConvertConst(op, srcW, dstW, c->getValue(), r))
				return nullptr;
			return constant(fn, destType, r);
		}

		Node* simplify(Function& fn, Node* n) {
			Opcode op = n->getOpcode();
			Node* r = nullptr;
			if(op == Opcode::Select) {
				SelectNode* s = cast<SelectNode>(n);
				if(s->getTrue() == s->getFalse())
					return s->getTrue();
				if(ConstantNode* c = dyn_cast<ConstantNode>(s->getCondition()))
					return c->getValue() != 0 ? s->getTrue() : s->getFalse();
			} else if(isBinaryOpcode(op)) {
				BinaryNode* b = cast<BinaryNode>(n);
				r = foldBinary(fn, op, b->getLHS(), b->getRHS());
			} else if(isUnaryOpcode(op)) {
				r = foldUnary(fn, op, cast<UnaryNode>(n)->getOperand());
			} else if(isCompareOpcode(op)) {
				CompareNode* c = cast<CompareNode>(n);
				r = foldCompare(fn, op, c->getLHS(), c->getRHS());
			} else if(isConvertOpcode(op)) {
				r = foldConvert(fn, op, cast<ConvertNode>(n)->getOperand(), n->getType());
			}
			return r ? r : n;
		}
	} // namespace detail

	inline void FoldPass::push(Node* n) {
		if(!isArithmeticOpcode(n->getOpcode()) && n->getOpcode() != Opcode::Select)
			return;
		U32 id = n->getId();
		if(id >= queued.size())
			queued.resize(id + 1, 0);
		if(queued[id])
			return;
		queued[id] = 1;
		work.push_back(n);
	}

	void FoldPass::pushFresh(Node* root) {
		stack.clear();
		if(root->getId() >= fresh)
			stack.push_back(root);
		U32 top = fresh;
		while(!stack.empty()) {
			Node* c = stack.back();
			stack.pop_back();
			if(c->getId() >= top)
				top = c->getId() + 1;
			push(c);
			for(U32 i = 0, e = c->getInputCount(); i < e; ++i) {
				Node* in = c->getInput(i);
				if(in && in->getId() >= fresh)
					stack.push_back(in);
			}
		}
		fresh = top;
	}

	U32 FoldPass::runOnFunction(Function& fn, const TargetInfo&) {
		U32 changed = 0;
		fresh = 0;
		work.clear();
		work.reserve(fn.size());
		queued.assign(fn.size(), 0);
		for(Node* n : fn) {
			if(n->getId() >= fresh)
				fresh = n->getId() + 1;
			push(n);
		}

		for(U32 i = 0; i < work.size(); ++i) {
			Node* n = work[i];
			queued[n->getId()] = 0;
			if(!n->hasUsers())
				continue;
			Node* s = detail::simplify(fn, n);
			if(s == n)
				continue;
			n->replaceAllUsesWith(s);
			++changed;
			push(s);
			for(Node* u : s->getUsers())
				push(u);
			pushFresh(s);
		}

		if(changed)
			fn.eliminateDeadNodes();
		return changed;
	}

	const C8* FoldPass::name() const { return "fold"; }
} // namespace rat
