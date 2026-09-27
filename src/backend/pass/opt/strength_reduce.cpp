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
			return dyn_cast<ConstantNode>(other);
		}
	} // namespace detail

	const C8* StrengthReducePass::name() const { return "strengthreduce"; }

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
