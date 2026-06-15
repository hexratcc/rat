#include "Pass/Opt/Fold.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

#include <cstdint>
#include <utility>

namespace rat {
	namespace {
		I64 signExtend(I64 v, U32 w) {
			if (w == 0 || w >= 64)
				return v;
			U64 mask = ((U64)1 << w) - 1;
			U64 m = (U64)1 << (w - 1);
			U64 x = (U64)v & mask;
			return (I64)((x ^ m) - m);
		}
		U64 maskW(I64 v, U32 w) {
			if (w >= 64)
				return (U64)v;
			return (U64)v & (((U64)1 << w) - 1);
		}
		I64 normalizeConst(I64 v, U32 w) {
			if (w == 1)
				return v & 1;
			return signExtend(v, w);
		}

		ConstantNode* asConstant(Node* n) {
			return n->getOpcode() == Opcode::Constant ? static_cast<ConstantNode*>(n)
																								: nullptr;
		}
		BinaryNode* asBinary(Node* n) {
			return isBinaryOpcode(n->getOpcode()) ? static_cast<BinaryNode*>(n)
																						: nullptr;
		}

		B32 isConstWithValue(Node* n, I64 want) {
			ConstantNode* c = asConstant(n);
			return c && c->getValue() == want;
		}
		B32 isZeroConst(Node* n) { return isConstWithValue(n, 0); }
		B32 isOneConst(Node* n) { return isConstWithValue(n, 1); }
		B32 isAllOnesConst(Node* n, U32 w) {
			ConstantNode* c = asConstant(n);
			return c && c->getValue() == normalizeConst(-1, w);
		}

		I32 pow2Log(Node* n, U32 w) {
			ConstantNode* c = asConstant(n);
			if (!c)
				return -1;
			U64 u = maskW(c->getValue(), w);
			if (u == 0 || (u & (u - 1)) != 0)
				return -1;
			I32 k = __builtin_ctzll(u);
			return (k >= 1 && k < (I32)w) ? k : -1;
		}

		B32 matchVarConst(Node* n, Opcode want, Node*& base, I64& c) {
			BinaryNode* b = asBinary(n);
			if (!b || b->getOpcode() != want)
				return false;
			if (ConstantNode* cr = asConstant(b->getRHS())) {
				base = b->getLHS();
				c = cr->getValue();
				return true;
			}
			if (want == Opcode::Add || want == Opcode::Mul)
				if (ConstantNode* cl = asConstant(b->getLHS())) {
					base = b->getRHS();
					c = cl->getValue();
					return true;
				}
			return false;
		}

		Node* constBool(Function& fn, B32 b) {
			return constant(fn, fn.types().getBool(), b ? 1 : 0);
		}
	} // namespace

	Node* constant(Function& fn, Type* type, I64 value) {
		return fn.create<ConstantNode>(type,
																	 normalizeConst(value, type->getIntWidth()));
	}

	Node* foldBinary(Function& fn, Opcode op, Node* lhs, Node* rhs) {
		Type* ty = lhs->getType();
		if (!ty->isInt())
			return nullptr;
		U32 w = ty->getIntWidth();

		ConstantNode* cl = asConstant(lhs);
		ConstantNode* cr = asConstant(rhs);

		if (cl && cr) {
			I64 a = cl->getValue(), b = cr->getValue();
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
					return k((I64)(maskW(a, w) >> b));
				return nullptr;
			case Opcode::AShr:
				if (b >= 0 && b < (I64)w)
					return k(signExtend(a, w) >> b);
				return nullptr;
			case Opcode::SDiv: {
				I64 sa = signExtend(a, w), sb = signExtend(b, w);
				if (sb == 0 || (sa == INT64_MIN && sb == -1))
					return nullptr;
				return k(sa / sb);
			}
			case Opcode::SRem: {
				I64 sa = signExtend(a, w), sb = signExtend(b, w);
				if (sb == 0 || (sa == INT64_MIN && sb == -1))
					return nullptr;
				return k(sa % sb);
			}
			case Opcode::UDiv: {
				U64 ua = maskW(a, w), ub = maskW(b, w);
				if (ub == 0)
					return nullptr;
				return k((I64)(ua / ub));
			}
			case Opcode::URem: {
				U64 ua = maskW(a, w), ub = maskW(b, w);
				if (ub == 0)
					return nullptr;
				return k((I64)(ua % ub));
			}
			default:
				return nullptr;
			}
		}

		// mormalize a lone constant to the RHS for commutative ops, so every rule
		// below only has to look on one side
		if (cl && !cr &&
				(op == Opcode::Add || op == Opcode::Mul || op == Opcode::And ||
				 op == Opcode::Or || op == Opcode::Xor)) {
			std::swap(lhs, rhs);
			std::swap(cl, cr);
		}

		switch (op) {
		case Opcode::Add:
			if (isZeroConst(rhs))
				return lhs;
			if (isZeroConst(lhs))
				return rhs;
			break;
		case Opcode::Sub:
			if (isZeroConst(rhs))
				return lhs;
			if (lhs == rhs)
				return constant(fn, ty, 0);
			break;
		case Opcode::Mul:
			if (isOneConst(rhs))
				return lhs;
			if (isOneConst(lhs))
				return rhs;
			if (isZeroConst(rhs) || isZeroConst(lhs))
				return constant(fn, ty, 0);
			break;
		case Opcode::And:
			if (lhs == rhs)
				return lhs;
			if (isZeroConst(rhs) || isZeroConst(lhs))
				return constant(fn, ty, 0);
			if (isAllOnesConst(rhs, w))
				return lhs;
			if (isAllOnesConst(lhs, w))
				return rhs;
			break;
		case Opcode::Or:
			if (lhs == rhs)
				return lhs;
			if (isZeroConst(rhs))
				return lhs;
			if (isZeroConst(lhs))
				return rhs;
			if (isAllOnesConst(rhs, w) || isAllOnesConst(lhs, w))
				return constant(fn, ty, normalizeConst(-1, w));
			break;
		case Opcode::Xor:
			if (isZeroConst(rhs))
				return lhs;
			if (isZeroConst(lhs))
				return rhs;
			if (lhs == rhs)
				return constant(fn, ty, 0);
			break;
		case Opcode::Shl:
		case Opcode::LShr:
		case Opcode::AShr:
			if (isZeroConst(rhs))
				return lhs;
			break;
		default:
			break;
		}

		auto mkBin = [&](Opcode o, Node* x, I64 c) {
			return fn.create<BinaryNode>(o, ty, x, constant(fn, ty, c));
		};

		// reassociation: gather constants in +/-/* chains
		if ((op == Opcode::Add || op == Opcode::Sub) && cr) {
			I64 c2 = cr->getValue();
			I32 outerSign = (op == Opcode::Add) ? 1 : -1;
			Node* base;
			I64 c1;
			for (Opcode inner : {Opcode::Add, Opcode::Sub}) {
				if (matchVarConst(lhs, inner, base, c1)) {
					I32 innerSign = (inner == Opcode::Add) ? 1 : -1;
					I64 k = normalizeConst(innerSign * c1 + outerSign * c2, w);
					if (k == 0)
						return base;
					return mkBin(Opcode::Add, base, k); // sub-by-const normalizes to add
				}
			}
		}
		if (op == Opcode::Mul && cr) {
			Node* base;
			I64 c1;
			if (matchVarConst(lhs, Opcode::Mul, base, c1)) {
				I64 k = normalizeConst(c1 * cr->getValue(), w);
				if (k == 0)
					return constant(fn, ty, 0);
				if (k == 1)
					return base;
				return mkBin(Opcode::Mul, base, k);
			}
		}

		// strength reduction
		switch (op) {
		case Opcode::Mul:
			if (I32 k = pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::Shl, lhs, k);
			break;
		case Opcode::UDiv:
			if (I32 k = pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::LShr, lhs, k);
			break;
		case Opcode::URem:
			if (I32 k = pow2Log(rhs, w); k > 0)
				return mkBin(Opcode::And, lhs, ((I64)1 << k) - 1);
			break;
		default:
			break;
		}

		// shift-of-shift collapse
		if (op == Opcode::Shl || op == Opcode::LShr || op == Opcode::AShr) {
			BinaryNode* in = asBinary(lhs);
			ConstantNode* cb = asConstant(rhs);
			if (in && in->getOpcode() == op && cb) {
				ConstantNode* ca = asConstant(in->getRHS());
				I64 a = ca ? ca->getValue() : -1;
				I64 b = cb->getValue();
				if (ca && a >= 0 && a < (I64)w && b >= 0 && b < (I64)w) {
					Node* x = in->getLHS();
					I64 sum = a + b;
					if (sum >= (I64)w)
						return op == Opcode::AShr ? mkBin(Opcode::AShr, x, w - 1)
																			: constant(fn, ty, 0);
					return mkBin(op, x, sum);
				}
			}
		}

		return nullptr;
	}

	Node* foldUnary(Function& fn, Opcode op, Node* operand) {
		if (ConstantNode* c = asConstant(operand)) {
			if (!operand->getType()->isInt())
				return nullptr;
			U32 w = operand->getType()->getIntWidth();
			I64 x = c->getValue();
			I64 r = (op == Opcode::Neg) ? -x : ~x;
			return constant(fn, operand->getType(), normalizeConst(r, w));
		}
		return nullptr;
	}

	Node* foldCompare(Function& fn, Opcode op, Node* lhs, Node* rhs) {
		Type* ty = lhs->getType();
		if (!ty->isInt())
			return nullptr;
		U32 w = ty->getIntWidth();

		ConstantNode* cl = asConstant(lhs);
		ConstantNode* cr = asConstant(rhs);
		if (cl && cr) {
			I64 a = cl->getValue(), b = cr->getValue();
			B32 res = false;
			switch (op) {
			case Opcode::Eq:
				res = a == b;
				break;
			case Opcode::Ne:
				res = a != b;
				break;
			case Opcode::Slt:
				res = signExtend(a, w) < signExtend(b, w);
				break;
			case Opcode::Sle:
				res = signExtend(a, w) <= signExtend(b, w);
				break;
			case Opcode::Ult:
				res = maskW(a, w) < maskW(b, w);
				break;
			case Opcode::Ule:
				res = maskW(a, w) <= maskW(b, w);
				break;
			default:
				return nullptr;
			}
			return constBool(fn, res);
		}

		if (lhs == rhs) {
			switch (op) {
			case Opcode::Eq:
			case Opcode::Sle:
			case Opcode::Ule:
				return constBool(fn, true);
			case Opcode::Ne:
			case Opcode::Slt:
			case Opcode::Ult:
				return constBool(fn, false);
			default:
				return nullptr;
			}
		}
		return nullptr;
	}

	Node* foldConvert(Function& fn, Opcode op, Node* operand, Type* destType) {
		if (operand->getType() == destType)
			return operand;
		ConstantNode* c = asConstant(operand);
		if (!c || !operand->getType()->isInt() || !destType->isInt())
			return nullptr;
		U32 srcW = operand->getType()->getIntWidth();
		U32 dstW = destType->getIntWidth();
		I64 x = c->getValue();
		I64 res = 0;
		switch (op) {
		case Opcode::Trunc:
			res = normalizeConst(x, dstW);
			break;
		case Opcode::SExt:
			res = normalizeConst(signExtend(x, srcW), dstW);
			break;
		case Opcode::ZExt:
			res = normalizeConst((I64)maskW(x, srcW), dstW);
			break;
		default:
			return nullptr;
		}
		return constant(fn, destType, res);
	}

	Node* simplify(Function& fn, Node* n) {
		Opcode op = n->getOpcode();
		if (isBinaryOpcode(op)) {
			BinaryNode* b = static_cast<BinaryNode*>(n);
			if (Node* r = foldBinary(fn, op, b->getLHS(), b->getRHS()))
				return r;
		} else if (isUnaryOpcode(op)) {
			UnaryNode* u = static_cast<UnaryNode*>(n);
			if (Node* r = foldUnary(fn, op, u->getOperand()))
				return r;
		} else if (isCompareOpcode(op)) {
			CompareNode* c = static_cast<CompareNode*>(n);
			if (Node* r = foldCompare(fn, op, c->getLHS(), c->getRHS()))
				return r;
		} else if (isConvertOpcode(op)) {
			ConvertNode* c = static_cast<ConvertNode*>(n);
			if (Node* r = foldConvert(fn, op, c->getOperand(), n->getType()))
				return r;
		}
		return n;
	}

	namespace {
		U32 foldFunction(Function& fn) {
			U32 changed = 0;
			B32 again = true;
			while (again) {
				again = false;
				// snapshot value nodes: simplify may append constants to the node
				// list, which would invalidate a live iterator (?)
				List<Node*> work;
				for (Node* n : fn) {
					Opcode op = n->getOpcode();
					if (isBinaryOpcode(op) || isUnaryOpcode(op) || isCompareOpcode(op) ||
							isConvertOpcode(op))
						work.push_back(n);
				}
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
	} // namespace

	const char* FoldPass::name() const { return "fold"; }

	B32 FoldPass::run(Module& module) {
		U32 changed = 0;
		for (Function* fn : module)
			changed += foldFunction(*fn);
		return changed != 0;
	}
} // namespace rat
