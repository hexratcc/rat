#include "Pass/Opt/Fold.h"

#include "IR/Function.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	I64 FoldPass::signExtend(I64 v, U32 w) {
		if (w == 0 || w >= 64)
			return v;
		U64 mask = ((U64)1 << w) - 1;
		U64 m = (U64)1 << (w - 1);
		U64 x = (U64)v & mask;
		return (I64)((x ^ m) - m);
	}

	U64 FoldPass::maskW(I64 v, U32 w) {
		if (w >= 64)
			return (U64)v;
		return (U64)v & (((U64)1 << w) - 1);
	}

	B32 FoldPass::wouldSignedDivOverflow(I64 a, I64 b) {
		return b == 0 || (a == INT64_MIN && b == -1);
	}

	I64 FoldPass::normalizeConst(I64 v, U32 w) {
		if (w == 1)
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
		if (!c)
			return -1;
		U64 u = maskW(c->getValue(), w);
		if (u == 0 || (u & (u - 1)) != 0)
			return -1;
		I32 k = __builtin_ctzll(u);
		return (k >= 1 && k < (I32)w) ? k : -1;
	}

	B32 FoldPass::matchVarConst(Node* n, Opcode want, Node*& base, I64& c) {
		BinaryNode* b = dyn_cast<BinaryNode>(n);
		if (!b || b->getOpcode() != want)
			return false;
		if (ConstantNode* cr = dyn_cast<ConstantNode>(b->getRHS())) {
			base = b->getLHS();
			c = cr->getValue();
			return true;
		}
		if (want == Opcode::Add || want == Opcode::Mul)
			if (ConstantNode* cl = dyn_cast<ConstantNode>(b->getLHS())) {
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
		switch (op) {
		case Opcode::Add:
			return k(a + b);
		case Opcode::Sub:
			return k(a - b);
		case Opcode::Mul:
			return k(a * b);
		case Opcode::And:
			return k(a & b);
		case Opcode::Or:
			return k(a | b);
		case Opcode::Xor:
			return k(a ^ b);
		case Opcode::Shl:
			if (b >= 0 && b < (I64)w)
				return k((I64)((U64)a << b));
			return nullptr;
		case Opcode::LShr:
			if (b >= 0 && b < (I64)w)
				return k((I64)(FoldPass::maskW(a, w) >> b));
			return nullptr;
		case Opcode::AShr:
			if (b >= 0 && b < (I64)w)
				return k(FoldPass::signExtend(a, w) >> b);
			return nullptr;
		case Opcode::SDiv: {
			I64 sa = FoldPass::signExtend(a, w), sb = FoldPass::signExtend(b, w);
			if (FoldPass::wouldSignedDivOverflow(sa, sb))
				return nullptr;
			return k(sa / sb);
		}
		case Opcode::SRem: {
			I64 sa = FoldPass::signExtend(a, w), sb = FoldPass::signExtend(b, w);
			if (FoldPass::wouldSignedDivOverflow(sa, sb))
				return nullptr;
			return k(sa % sb);
		}
		case Opcode::UDiv: {
			U64 ua = FoldPass::maskW(a, w), ub = FoldPass::maskW(b, w);
			if (ub == 0)
				return nullptr;
			return k((I64)(ua / ub));
		}
		case Opcode::URem: {
			U64 ua = FoldPass::maskW(a, w), ub = FoldPass::maskW(b, w);
			if (ub == 0)
				return nullptr;
			return k((I64)(ua % ub));
		}
		default:
			return nullptr;
		}
	}

	Node* foldBinaryIdentity(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
		switch (op) {
		case Opcode::Add:
			if (FoldPass::isZeroConst(rhs))
				return lhs;
			if (FoldPass::isZeroConst(lhs))
				return rhs;
			break;
		case Opcode::Sub:
			if (FoldPass::isZeroConst(rhs))
				return lhs;
			if (lhs == rhs)
				return constant(fn, ty, 0);
			break;
		case Opcode::Mul:
			if (FoldPass::isOneConst(rhs))
				return lhs;
			if (FoldPass::isOneConst(lhs))
				return rhs;
			if (FoldPass::isZeroConst(rhs) || FoldPass::isZeroConst(lhs))
				return constant(fn, ty, 0);
			break;
		case Opcode::And:
			if (lhs == rhs)
				return lhs;
			if (FoldPass::isZeroConst(rhs) || FoldPass::isZeroConst(lhs))
				return constant(fn, ty, 0);
			if (FoldPass::isAllOnesConst(rhs, w))
				return lhs;
			if (FoldPass::isAllOnesConst(lhs, w))
				return rhs;
			break;
		case Opcode::Or:
			if (lhs == rhs)
				return lhs;
			if (FoldPass::isZeroConst(rhs))
				return lhs;
			if (FoldPass::isZeroConst(lhs))
				return rhs;
			if (FoldPass::isAllOnesConst(rhs, w) || FoldPass::isAllOnesConst(lhs, w))
				return constant(fn, ty, FoldPass::normalizeConst(-1, w));
			break;
		case Opcode::Xor:
			if (FoldPass::isZeroConst(rhs))
				return lhs;
			if (FoldPass::isZeroConst(lhs))
				return rhs;
			if (lhs == rhs)
				return constant(fn, ty, 0);
			break;
		case Opcode::Shl:
		case Opcode::LShr:
		case Opcode::AShr:
			if (FoldPass::isZeroConst(rhs))
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
		if ((op == Opcode::Add || op == Opcode::Sub) && cr) {
			I64 c2 = cr->getValue();
			I32 outerSign = (op == Opcode::Add) ? 1 : -1;
			Node* base;
			I64 c1;
			for (Opcode inner : {Opcode::Add, Opcode::Sub}) {
				if (FoldPass::matchVarConst(lhs, inner, base, c1)) {
					I32 innerSign = (inner == Opcode::Add) ? 1 : -1;
					I64 k = FoldPass::normalizeConst(innerSign * c1 + outerSign * c2, w);
					if (k == 0)
						return base;
					return mkBin(Opcode::Add, base, k); // sub-by-const normalizes to add
				}
			}
		}
		if (op == Opcode::Mul && cr) {
			Node* base;
			I64 c1;
			if (FoldPass::matchVarConst(lhs, Opcode::Mul, base, c1)) {
				I64 k = FoldPass::normalizeConst(c1 * cr->getValue(), w);
				if (k == 0)
					return constant(fn, ty, 0);
				if (k == 1)
					return base;
				return mkBin(Opcode::Mul, base, k);
			}
		}
		return nullptr;
	}

	Node* foldBinaryStrength(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
		auto mkBin = [&](Opcode o, Node* x, I64 c) {
			return fn.create<BinaryNode>(o, ty, x, constant(fn, ty, c));
		};
		switch (op) {
		case Opcode::Mul:
			if (I32 k = FoldPass::pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::Shl, lhs, k);
			break;
		case Opcode::UDiv:
			if (I32 k = FoldPass::pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::LShr, lhs, k);
			break;
		case Opcode::URem:
			if (I32 k = FoldPass::pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::And, lhs, ((I64)1 << k) - 1);
			break;
		default:
			break;
		}
		return nullptr;
	}

	Node* foldShiftOfShift(Function& fn, Opcode op, Type* ty, U32 w, Node* lhs, Node* rhs) {
		if (op != Opcode::Shl && op != Opcode::LShr && op != Opcode::AShr)
			return nullptr;
		auto mkBin = [&](Opcode o, Node* x, I64 c) {
			return fn.create<BinaryNode>(o, ty, x, constant(fn, ty, c));
		};
		BinaryNode* in = dyn_cast<BinaryNode>(lhs);
		ConstantNode* cb = dyn_cast<ConstantNode>(rhs);
		if (in && in->getOpcode() == op && cb) {
			ConstantNode* ca = dyn_cast<ConstantNode>(in->getRHS());
			I64 a = ca ? ca->getValue() : -1;
			I64 b = cb->getValue();
			if (ca && a >= 0 && a < (I64)w && b >= 0 && b < (I64)w) {
				Node* x = in->getLHS();
				I64 sum = a + b;
				if (sum >= (I64)w)
					return op == Opcode::AShr ? mkBin(Opcode::AShr, x, w - 1) : constant(fn, ty, 0);
				return mkBin(op, x, sum);
			}
		}
		return nullptr;
	}

	Node* foldBinary(Function& fn, Opcode op, Node* lhs, Node* rhs) {
		Type* ty = lhs->getType();
		if (!ty->isInt())
			return nullptr;
		U32 w = ty->getIntWidth();

		ConstantNode* cl = dyn_cast<ConstantNode>(lhs);
		ConstantNode* cr = dyn_cast<ConstantNode>(rhs);

		if (cl && cr)
			return foldBinaryConst(fn, op, ty, w, cl->getValue(), cr->getValue());

		// normalize a lone constant to the RHS for commutative ops, so every rule
		// below only has to look on one side
		if (cl && !cr &&
				(op == Opcode::Add || op == Opcode::Mul || op == Opcode::And || op == Opcode::Or ||
				 op == Opcode::Xor)) {
			std::swap(lhs, rhs);
			std::swap(cl, cr);
		}

		if (Node* r = foldBinaryIdentity(fn, op, ty, w, lhs, rhs))
			return r;
		if (Node* r = foldBinaryReassoc(fn, op, ty, w, lhs, cr))
			return r;
		if (Node* r = foldBinaryStrength(fn, op, ty, w, lhs, rhs))
			return r;
		if (Node* r = foldShiftOfShift(fn, op, ty, w, lhs, rhs))
			return r;
		return nullptr;
	}

	Node* foldUnary(Function& fn, Opcode op, Node* operand) {
		if (ConstantNode* c = dyn_cast<ConstantNode>(operand)) {
			if (!operand->getType()->isInt())
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
		if (!ty->isInt())
			return nullptr;
		U32 w = ty->getIntWidth();

		ConstantNode* cl = dyn_cast<ConstantNode>(lhs);
		ConstantNode* cr = dyn_cast<ConstantNode>(rhs);
		if (cl && cr) {
			I64 a = cl->getValue(), b = cr->getValue();
			B32 res = false;
			switch (op) {
			case Opcode::Eq:
				res = FoldPass::maskW(a, w) == FoldPass::maskW(b, w);
				break;
			case Opcode::Ne:
				res = FoldPass::maskW(a, w) != FoldPass::maskW(b, w);
				break;
			case Opcode::Slt:
				res = FoldPass::signExtend(a, w) < FoldPass::signExtend(b, w);
				break;
			case Opcode::Sle:
				res = FoldPass::signExtend(a, w) <= FoldPass::signExtend(b, w);
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

		if (lhs == rhs) {
			switch (op) {
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
		return nullptr;
	}

	Node* foldConvert(Function& fn, Opcode op, Node* operand, Type* destType) {
		if (operand->getType() == destType)
			return operand;
		ConstantNode* c = dyn_cast<ConstantNode>(operand);
		if (!c || !operand->getType()->isInt() || !destType->isInt())
			return nullptr;
		U32 srcW = operand->getType()->getIntWidth();
		U32 dstW = destType->getIntWidth();
		I64 x = c->getValue();
		I64 res = 0;
		switch (op) {
		case Opcode::Trunc:
			res = FoldPass::normalizeConst(x, dstW);
			break;
		case Opcode::SExt:
			res = FoldPass::normalizeConst(FoldPass::signExtend(x, srcW), dstW);
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
		if (isBinaryOpcode(op)) {
			BinaryNode* b = cast<BinaryNode>(n);
			if (Node* r = foldBinary(fn, op, b->getLHS(), b->getRHS()))
				return r;
		} else if (isUnaryOpcode(op)) {
			UnaryNode* u = cast<UnaryNode>(n);
			if (Node* r = foldUnary(fn, op, u->getOperand()))
				return r;
		} else if (isCompareOpcode(op)) {
			CompareNode* c = cast<CompareNode>(n);
			if (Node* r = foldCompare(fn, op, c->getLHS(), c->getRHS()))
				return r;
		} else if (isConvertOpcode(op)) {
			ConvertNode* c = cast<ConvertNode>(n);
			if (Node* r = foldConvert(fn, op, c->getOperand(), n->getType()))
				return r;
		}
		return n;
	}

	U32 FoldPass::foldFunction(Function& fn) {
		U32 changed = 0;
		B32 again = true;
		while (again) {
			again = false;
			List<Node*> work;
			for (Node* n : fn)
				if (isArithmeticOpcode(n->getOpcode()))
					work.push_back(n);
			for (Node* n : work) {
				if (!n->hasUsers())
					continue;
				Node* s = simplify(fn, n);
				if (s != n) {
					n->replaceAllUsesWith(s);
					++changed;
					again = true;
				}
			}
		}
		if (changed)
			fn.eliminateDeadNodes();
		return changed;
	}

	const C8* FoldPass::name() const { return "fold"; }

	U32 FoldPass::runOnFunction(Function& fn) { return foldFunction(fn); }
} // namespace rat
