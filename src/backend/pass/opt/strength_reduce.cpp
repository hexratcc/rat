#include "pass/opt/strength_reduce.h"

#include "ir/function.h"
#include "ir/node.h"
#include "pass/opt/fold.h"

namespace rat {
	namespace detail {
		B32 matchLinearIV(PhiNode* p, I64& step, U32& recIdx) {
			if(!p->getType()->isInt() || p->getValueCount() != 2)
				return false;
			for(U32 i = 0; i < 2; ++i) {
				Node* v = p->getValue(i);
				if(!v || v->getOpcode() != Opcode::Add)
					continue;
				Node* a = v->getInput(0);
				Node* b = v->getInput(1);
				ConstantNode* c = nullptr;
				if(a == p)
					c = dyn_cast<ConstantNode>(b);
				else if(b == p)
					c = dyn_cast<ConstantNode>(a);
				if(!c)
					continue;
				step = c->getValue();
				recIdx = i;
				return true;
			}
			return false;
		}

		B32 foldableScale(Opcode op, I64 k, U32 width) {
			if(width != 64)
				return false;
			if(op == Opcode::Shl)
				return k >= 1 && k <= 3;
			return k == 2 || k == 4 || k == 8;
		}

		ConstantNode* affineScale(Node* u, PhiNode* p) {
			if(u->getInputCount() != 2)
				return nullptr;
			Opcode op = u->getOpcode();
			if(op != Opcode::Mul && op != Opcode::Shl)
				return nullptr;
			if(u->getType() != p->getType())
				return nullptr;
			if(op == Opcode::Shl && u->getInput(0) != p)
				return nullptr; // k << p is not affine in p
			if(u->getInput(0) == p && u->getInput(1) == p)
				return nullptr; // p * p is quadratic
			Node* other = u->getInput(0) == p ? u->getInput(1) : u->getInput(0);
			ConstantNode* c = dyn_cast<ConstantNode>(other);
			if(c && foldableScale(op, c->getValue(), p->getType()->getIntWidth()))
				return nullptr;
			return c;
		}

		// p < c or p <= c for a constant c, whose then arm is the back edge of p
		CompareNode* loopTest(PhiNode* p, U32 recIdx) {
			ProjNode* thenP = dyn_cast<ProjNode>(p->getRegion()->getPredecessor(recIdx));
			if(!thenP || thenP->getIndex() != IfNode::thenProjIndex())
				return nullptr;
			IfNode* iff = dyn_cast<IfNode>(thenP->getProducer());
			if(!iff || iff->getControl() != p->getRegion())
				return nullptr;
			CompareNode* cmp = dyn_cast<CompareNode>(iff->getPredicate());
			if(!cmp || cmp->getLHS() != p || !isa<ConstantNode>(cmp->getRHS()))
				return nullptr;
			Opcode op = cmp->getOpcode();
			if(op != Opcode::Slt && op != Opcode::Sle)
				return nullptr;
			return cmp;
		}

		// k of u = p + k (null otherwise)
		ConstantNode* offsetOf(Node* u, PhiNode* p) {
			if(u->getOpcode() != Opcode::Add)
				return nullptr;
			if(u->getInput(0) == p)
				return dyn_cast<ConstantNode>(u->getInput(1));
			if(u->getInput(1) == p)
				return dyn_cast<ConstantNode>(u->getInput(0));
			return nullptr;
		}

		B32 isSext64(Node* u) {
			return u->getOpcode() == Opcode::SExt && u->getType()->getIntWidth() == 64;
		}

		B32 widenableUse(Node* u, PhiNode* p, I64 lo, I64 top) {
			if(isSext64(u))
				return true;
			ConstantNode* c = offsetOf(u, p);
			if(!c)
				return false;
			I64 k = signExtend(c->getValue(), 32);
			B32 fits = lo + k >= INT32_MIN && top + k <= INT32_MAX;
			for(Node* s : u->getUsers())
				if(s != p && (!isSext64(s) || !fits))
					return false;
			return true;
		}

		// make u read q, the i64 IV of p
		void widenUse(Function& fn, Node* u, PhiNode* p, PhiNode* q, CompareNode* cmp, I64 bound) {
			Type* i64 = q->getType();
			if(u == cmp) {
				Node* rhs = constant(fn, i64, bound);
				cmp->replaceAllUsesWith(fn.create<CompareNode>(cmp->getOpcode(), cmp->getType(), q, rhs));
			} else if(ConstantNode* c = offsetOf(u, p)) {
				Node* k = constant(fn, i64, signExtend(c->getValue(), 32));
				Node* sum = fn.create<BinaryNode>(Opcode::Add, i64, q, k);
				List<Node*> sexts(u->getUsers().begin(), u->getUsers().end());
				for(Node* s : sexts)
					if(s != p)
						s->replaceAllUsesWith(sum);
			} else {
				u->replaceAllUsesWith(q);
			}
		}
	} // namespace detail

	const C8* StrengthReducePass::name() const { return "strengthreduce"; }

	// turn an i32 IV read only by its loop test, sexts and p + k sums into an i64 IV
	U32 StrengthReducePass::widen(Function& fn, PhiNode* p, I64 step, U32 recIdx) {
		CompareNode* cmp = detail::loopTest(p, recIdx);
		Node* init = p->getValue(1 - recIdx);
		if(!cmp || !isa<ConstantNode>(init) || p->getType()->getIntWidth() != 32)
			return 0;
		I64 lo = signExtend(cast<ConstantNode>(init)->getValue(), 32);
		I64 bound = signExtend(cast<ConstantNode>(cmp->getRHS())->getValue(), 32);
		I64 limit = bound;
		if(cmp->getOpcode() == Opcode::Sle)
			limit = bound + 1;
		step = signExtend(step, 32);
		I64 top = std::max(lo, limit - 1 + step);
		if(step <= 0 || top > INT32_MAX)
			return 0;
		for(Node* u : p->getUsers())
			if(u != cmp && !detail::widenableUse(u, p, lo, top))
				return 0;

		Type* i64 = fn.types().getInt(64);
		Node* init64 = constant(fn, i64, lo);
		PhiNode* q = fn.create<PhiNode>(i64, List<Node*>{p->getRegion(), init64, init64});
		q->setInput(1 + recIdx, fn.create<BinaryNode>(Opcode::Add, i64, q, constant(fn, i64, step)));
		List<Node*> users(p->getUsers().begin(), p->getUsers().end());
		for(Node* u : users)
			detail::widenUse(fn, u, p, q, cmp, bound);
		p->setInput(1 + recIdx, init); // p and its step are dead, cut their cycle
		return 1;
	}

	U32 StrengthReducePass::runOnFunction(Function& fn, const TargetInfo&) {
		U32 changed = 0;

		List<PhiNode*> phis;
		for(Node* n : fn)
			if(PhiNode* p = dyn_cast<PhiNode>(n))
				phis.push_back(p);

		for(PhiNode* p : phis) {
			I64 step;
			U32 recIdx;
			if(!detail::matchLinearIV(p, step, recIdx))
				continue;
			if(U32 widened = widen(fn, p, step, recIdx)) {
				changed += widened;
				continue;
			}
			Type* ty = p->getType();
			U32 w = ty->getIntWidth();
			Node* init = p->getValue(1 - recIdx);
			RegionNode* region = p->getRegion();

			// collect candidate uses
			List<Node*> muls;
			for(Node* u : p->getUsers())
				if(detail::affineScale(u, p))
					muls.push_back(u);

			for(Node* mul : muls) {
				Opcode op = mul->getOpcode();
				I64 k = detail::affineScale(mul, p)->getValue();
				I64 scaledStep;
				if(!evalBinaryConst(op, w, step, k, scaledStep))
					continue;

				Node* scaledInit = fn.create<BinaryNode>(op, ty, init, constant(fn, ty, k));
				PhiNode* q = fn.create<PhiNode>(ty, List<Node*>{region, scaledInit, scaledInit});
				Node* stepNode = fn.create<BinaryNode>(Opcode::Add, ty, q, constant(fn, ty, scaledStep));
				q->setInput(1 + recIdx, stepNode);

				mul->replaceAllUsesWith(q);
				++changed;
			}
		}

		if(changed)
			fn.eliminateDeadNodes();
		return changed;
	}
} // namespace rat
