#include "pass/opt/sccp.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "pass/opt/fold.h"

namespace rat {
	SCCPPass::Lattice SCCPPass::Lattice::top() { return {Kind::Top, 0}; }
	SCCPPass::Lattice SCCPPass::Lattice::bottom() { return {Kind::Bottom, 0}; }
	SCCPPass::Lattice SCCPPass::Lattice::constant(I64 v) { return {Kind::Constant, v}; }

	B32 SCCPPass::Lattice::operator==(const Lattice& o) const {
		return kind == o.kind && (kind != Kind::Constant || value == o.value);
	}

	B32 SCCPPass::Lattice::operator!=(const Lattice& o) const { return !(*this == o); }

	B32 SCCPPass::isValueNode(Node* n) {
		Opcode op = n->getOpcode();
		return op == Opcode::Constant || op == Opcode::Phi || isArithmeticOpcode(op);
	}

	SCCPPass::Lattice SCCPPass::meet(Lattice a, Lattice b) {
		if(a.kind == Lattice::Kind::Top)
			return b;
		if(b.kind == Lattice::Kind::Top)
			return a;
		if(a.kind == Lattice::Kind::Bottom || b.kind == Lattice::Kind::Bottom)
			return Lattice::bottom();
		return a.value == b.value ? a : Lattice::bottom();
	}

	void SCCPPass::ensureSize(U32 id) {
		if(id >= values.size()) {
			values.resize(id + 1);
			exec.resize(id + 1, 0);
		}
	}

	B32 SCCPPass::isExec(Node* c) const { return c && c->getId() < exec.size() && exec[c->getId()]; }

	SCCPPass::Lattice SCCPPass::get(Node* n) {
		if(ConstantNode* c = dyn_cast<ConstantNode>(n))
			return Lattice::constant(c->getValue());
		if(!isValueNode(n))
			return Lattice::bottom();
		U32 id = n->getId();
		return id < values.size() ? values[id] : Lattice::top();
	}

	void SCCPPass::markExec(Node* c) {
		U32 id = c->getId();
		ensureSize(id);
		if(!exec[id]) {
			exec[id] = 1;
			flowWork.push_back(c);
		}
	}

	void SCCPPass::pushPhis(RegionNode* r) {
		for(PhiNode* phi : usersOfType<PhiNode>(r))
			ssaWork.push_back(phi);
	}

	void SCCPPass::evalIf(IfNode* iff) {
		if(!isExec(iff))
			return;
		Lattice p = get(iff->getPredicate());
		if(p.kind == Lattice::Kind::Top)
			return;
		B32 takeThen = p.kind == Lattice::Kind::Bottom || p.value != 0;
		B32 takeElse = p.kind == Lattice::Kind::Bottom || p.value == 0;
		if(takeThen)
			if(ProjNode* t = iff->projection(IfNode::thenProjIndex()))
				markExec(t);
		if(takeElse)
			if(ProjNode* e = iff->projection(IfNode::elseProjIndex()))
				markExec(e);
	}

	void SCCPPass::visitFlow(Node* c) {
		for(Node* u : c->getUsers()) {
			// a call or an asm passes control on through its control projection
			if(isa<CallNode>(u) || isa<AsmNode>(u)) {
				if(u->getControlInput() == c)
					if(ProjNode* cp = u->projection(CallNode::controlProjIndex()))
						markExec(cp);
				continue;
			}
			if(!isControlNode(u))
				continue;
			switch(u->getOpcode()) {
			case Opcode::Proj:
				if(c->getOpcode() != Opcode::If)
					markExec(u);
				break;
			case Opcode::Region:
				markExec(u);
				pushPhis(cast<RegionNode>(u));
				break;
			case Opcode::If:
			case Opcode::Switch:
				if(u->getControlInput() == c)
					markExec(u);
				break;
			default:
				markExec(u);
				break;
			}
		}
		if(IfNode* iff = dyn_cast<IfNode>(c))
			evalIf(iff);
	}

	void SCCPPass::visitSSA(Node* n) {
		if(IfNode* iff = dyn_cast<IfNode>(n)) {
			evalIf(iff);
			return;
		}
		if(!isValueNode(n))
			return;
		Lattice nw = evaluate(n);
		if(nw != get(n)) {
			U32 id = n->getId();
			ensureSize(id);
			values[id] = nw;
			for(Node* u : n->getUsers())
				ssaWork.push_back(u);
		}
	}

	SCCPPass::Lattice SCCPPass::evaluate(Node* n) {
		Opcode op = n->getOpcode();
		if(op == Opcode::Constant)
			return Lattice::constant(cast<ConstantNode>(n)->getValue());
		if(op == Opcode::Phi)
			return evalPhi(cast<PhiNode>(n));
		return evalArith(n);
	}

	SCCPPass::Lattice SCCPPass::evalPhi(PhiNode* phi) {
		RegionNode* r = dyn_cast<RegionNode>(phi->getInput(0));
		if(!r)
			return Lattice::bottom();
		Lattice res = Lattice::top();
		for(U32 i = 0, e = phi->getValueCount(); i < e; ++i) {
			if(!isExec(r->getPredecessor(i)))
				continue;
			res = meet(res, get(phi->getValue(i)));
			if(res.kind == Lattice::Kind::Bottom)
				break;
		}
		return res;
	}

	SCCPPass::Lattice SCCPPass::evalArith(Node* n) {
		Opcode op = n->getOpcode();
		Node* lhs = n->getInput(0);
		Node* rhs = n->getInputCount() > 1 ? n->getInput(1) : nullptr;
		Lattice a = get(lhs);
		Lattice b = rhs ? get(rhs) : Lattice::constant(0);
		if(a.kind == Lattice::Kind::Bottom || b.kind == Lattice::Kind::Bottom)
			return Lattice::bottom();
		if(a.kind == Lattice::Kind::Top || b.kind == Lattice::Kind::Top)
			return Lattice::top();

		Type* lty = lhs->getType();
		if(isConvertOpcode(op) && lty == n->getType())
			return Lattice::constant(a.value);
		if(!lty->isInt())
			return Lattice::bottom();
		U32 w = lty->getIntWidth();
		I64 out;
		if(isBinaryOpcode(op)) {
			if(evalBinaryConst(op, w, a.value, b.value, out))
				return Lattice::constant(out);
		} else if(isUnaryOpcode(op)) {
			if(evalUnaryConst(op, w, a.value, out))
				return Lattice::constant(out);
		} else if(isCompareOpcode(op)) {
			if(evalCompareConst(op, w, a.value, b.value, out))
				return Lattice::constant(out);
		} else if(isConvertOpcode(op)) {
			Type* dty = n->getType();
			if(dty->isInt() && evalConvertConst(op, w, dty->getIntWidth(), a.value, out))
				return Lattice::constant(out);
		}
		return Lattice::bottom();
	}

	void SCCPPass::solve(Function& fn) {
		markExec(fn.getStart());
		for(Node* n : fn)
			if(isValueNode(n))
				ssaWork.push_back(n);
		while(!flowWork.empty() || !ssaWork.empty()) {
			while(!flowWork.empty()) {
				Node* c = flowWork.back();
				flowWork.pop_back();
				visitFlow(c);
			}
			while(!ssaWork.empty()) {
				Node* n = ssaWork.back();
				ssaWork.pop_back();
				visitSSA(n);
			}
		}
	}

	U32 SCCPPass::rewrite(Function& fn) {
		U32 changed = 0;
		List<Node*> work;
		for(Node* n : fn)
			work.push_back(n);
		for(Node* n : work) {
			if(isa<ConstantNode>(n) || !isValueNode(n) || !n->hasUsers())
				continue;
			Lattice l = get(n);
			if(l.kind != Lattice::Kind::Constant)
				continue;
			n->replaceAllUsesWith(constant(fn, n->getType(), l.value));
			++changed;
		}
		if(changed)
			fn.eliminateDeadNodes();
		return changed;
	}

	const C8* SCCPPass::name() const { return "sccp"; }

	U32 SCCPPass::runOnFunction(Function& fn, const TargetInfo&) {
		values.clear();
		exec.clear();
		flowWork.clear();
		ssaWork.clear();
		ensureSize(fn.size());
		solve(fn);
		return rewrite(fn);
	}
} // namespace rat
