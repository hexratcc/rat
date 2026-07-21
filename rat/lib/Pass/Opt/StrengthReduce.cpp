#include "Pass/Opt/StrengthReduce.h"

#include "IR/Function.h"
#include "IR/Node.h"
#include "Pass/Opt/Fold.h"

namespace rat {
	namespace {
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
	} // namespace

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
			if(!matchLinearIV(p, step, recIdx))
				continue;
			Type* ty = p->getType();
			U32 w = ty->getIntWidth();
			Node* init = p->getValue(1 - recIdx);
			RegionNode* region = p->getRegion();

			// collect candidate uses
			List<Node*> muls;
			for(Node* u : p->getUsers()) {
				if(u->getInputCount() != 2)
					continue;
				Opcode op = u->getOpcode();
				if(op != Opcode::Mul && op != Opcode::Shl)
					continue;
				if(u->getType() != ty)
					continue;
				Node* other = u->getInput(0) == p ? u->getInput(1) : u->getInput(0);
				if(op == Opcode::Shl && u->getInput(0) != p)
					continue; // k << p is not affine in p
				if(u->getInput(0) == p && u->getInput(1) == p)
					continue; // p * p is quadratic
				if(!isa<ConstantNode>(other))
					continue;
				muls.push_back(u);
			}

			for(Node* mul : muls) {
				Opcode op = mul->getOpcode();
				Node* other = mul->getInput(0) == p ? mul->getInput(1) : mul->getInput(0);
				I64 k = cast<ConstantNode>(other)->getValue();
				I64 scaledStep;
				if(!evalBinaryConst(op, w, step, k, scaledStep))
					continue;

				Node* scaledInit = fn.create<BinaryNode>(op, ty, init, constant(fn, ty, k));
				List<Node*> inputs = {region, nullptr, nullptr};
				inputs[1 + (1 - recIdx)] = scaledInit;
				inputs[1 + recIdx] = scaledInit; // placeholder
				PhiNode* q = fn.create<PhiNode>(ty, inputs);
				Node* stepNode =
						fn.create<BinaryNode>(Opcode::Add, ty, q, constant(fn, ty, scaledStep));
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
