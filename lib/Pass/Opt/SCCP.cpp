// sparse conditional constant propagation: an optimistic data-flow solver that
// jointly discovers constants and which control edges can execute, so a value
// merged at a region (phi) is constant whenever every reachable predecessor
// agrees
//
// references:
// - M. N. Wegman and F. K. Zadeck, "Constant Propagation with Conditional
//   Branches", ACM TOPLAS, 1991
// - C. Click, "Combining Analyses, Combining Optimizations", PhD thesis,
//   Rice University, 1995 (the sea-of-nodes formulation)

#include "Pass/Opt/SCCP.h"

#include "IR/Function.h"
#include "IR/Node.h"
#include "IR/Type.h"
#include "Pass/Opt/Fold.h"

namespace rat {
	namespace detail {
		enum class LatKind : U8 { Top, Const, Bottom };

		struct Lat {
			LatKind kind = LatKind::Top;
			I64 value = 0;

			static Lat top() { return {LatKind::Top, 0}; }
			static Lat con(I64 v) { return {LatKind::Const, v}; }
			static Lat bot() { return {LatKind::Bottom, 0}; }

			B32 operator==(const Lat& o) const {
				return kind == o.kind && (kind != LatKind::Const || value == o.value);
			}
			B32 operator!=(const Lat& o) const { return !(*this == o); }
		};

		struct SCCPSolver {
			Function& fn;
			Map<Node*, Lat> values; // lattice for value-producing nodes
			Set<Node*> exec;				// control nodes proven executable
			List<Node*> flowWork;		// executable control nodes to propagate
			List<Node*> ssaWork;		// value / If nodes to re-evaluate

			SCCPSolver(Function& fn) : fn(fn) {}

			static B32 isValueNode(Node* n) {
				Opcode op = n->getOpcode();
				return op == Opcode::Constant || op == Opcode::Phi ||
							 isBinaryOpcode(op) || isUnaryOpcode(op) || isCompareOpcode(op) ||
							 isConvertOpcode(op);
			}

			static B32 isControlFlow(Node* n) {
				switch (n->getOpcode()) {
				case Opcode::Start:
				case Opcode::Stop:
				case Opcode::Return:
				case Opcode::Region:
				case Opcode::If:
					return true;
				case Opcode::Proj:
					return n->getType()->isControl();
				default:
					return false;
				}
			}

			static Lat meet(Lat a, Lat b) {
				if (a.kind == LatKind::Top)
					return b;
				if (b.kind == LatKind::Top)
					return a;
				if (a.kind == LatKind::Bottom || b.kind == LatKind::Bottom)
					return Lat::bot();
				return a.value == b.value ? a : Lat::bot();
			}

			Lat get(Node* n) {
				if (ConstantNode* c = dyn_cast<ConstantNode>(n))
					return Lat::con(c->getValue());
				if (!isValueNode(n))
					return Lat::bot();
				auto it = values.find(n);
				return it == values.end() ? Lat::top() : it->second;
			}

			void markExec(Node* c) {
				if (exec.insert(c).second)
					flowWork.push_back(c);
			}

			void pushPhis(RegionNode* r) {
				for (Node* u : r->getUsers())
					if (isa<PhiNode>(u))
						ssaWork.push_back(u);
			}

			void evalIf(IfNode* iff) {
				if (!exec.count(iff))
					return;
				Lat p = get(iff->getPredicate());
				if (p.kind == LatKind::Top)
					return;
				B32 takeThen = p.kind == LatKind::Bottom || p.value != 0;
				B32 takeElse = p.kind == LatKind::Bottom || p.value == 0;
				if (takeThen)
					if (ProjNode* t = iff->projection(IfNode::thenProjIndex()))
						markExec(t);
				if (takeElse)
					if (ProjNode* e = iff->projection(IfNode::elseProjIndex()))
						markExec(e);
			}

			void visitFlow(Node* c) {
				for (Node* u : c->getUsers()) {
					if (CallNode* call = dyn_cast<CallNode>(u)) {
						if (call->getControlInput() == c)
							if (ProjNode* cp = call->projection(CallNode::controlProjIndex()))
								markExec(cp);
						continue;
					}
					if (!isControlFlow(u))
						continue;
					switch (u->getOpcode()) {
					case Opcode::Proj:
						if (c->getOpcode() != Opcode::If)
							markExec(u);
						break;
					case Opcode::Region:
						markExec(u);
						pushPhis(cast<RegionNode>(u));
						break;
					case Opcode::If:
						if (u->getControlInput() == c)
							markExec(u);
						break;
					default:
						markExec(u);
						break;
					}
				}
				if (IfNode* iff = dyn_cast<IfNode>(c))
					evalIf(iff);
			}

			void visitSSA(Node* n) {
				if (IfNode* iff = dyn_cast<IfNode>(n)) {
					evalIf(iff);
					return;
				}
				if (!isValueNode(n))
					return;
				Lat nw = evaluate(n);
				if (nw != get(n)) {
					values[n] = nw;
					for (Node* u : n->getUsers())
						ssaWork.push_back(u);
				}
			}

			Lat evaluate(Node* n) {
				Opcode op = n->getOpcode();
				if (op == Opcode::Constant)
					return Lat::con(cast<ConstantNode>(n)->getValue());
				if (op == Opcode::Phi)
					return evalPhi(cast<PhiNode>(n));
				return evalArith(n);
			}

			Lat evalPhi(PhiNode* phi) {
				RegionNode* r = dyn_cast<RegionNode>(phi->getInput(0));
				if (!r)
					return Lat::bot();
				Lat res = Lat::top();
				for (U32 i = 0, e = phi->getValueCount(); i < e; ++i) {
					if (!exec.count(r->getPredecessor(i)))
						continue;
					res = meet(res, get(phi->getValue(i)));
					if (res.kind == LatKind::Bottom)
						break;
				}
				return res;
			}

			Lat evalArith(Node* n) {
				Opcode op = n->getOpcode();
				Node* lhs = n->getInput(0);
				Node* rhs = n->getInputCount() > 1 ? n->getInput(1) : nullptr;
				Lat a = get(lhs);
				Lat b = rhs ? get(rhs) : Lat::con(0);
				if (a.kind == LatKind::Bottom || b.kind == LatKind::Bottom)
					return Lat::bot();
				if (a.kind == LatKind::Top || b.kind == LatKind::Top)
					return Lat::top();

				Node* cl = constant(fn, lhs->getType(), a.value);
				Node* cr = rhs ? constant(fn, rhs->getType(), b.value) : nullptr;
				Node* res = nullptr;
				if (isBinaryOpcode(op))
					res = foldBinary(fn, op, cl, cr);
				else if (isUnaryOpcode(op))
					res = foldUnary(fn, op, cl);
				else if (isCompareOpcode(op))
					res = foldCompare(fn, op, cl, cr);
				else if (isConvertOpcode(op))
					res = foldConvert(fn, op, cl, n->getType());
				if (ConstantNode* c = dyn_cast<ConstantNode>(res))
					return Lat::con(c->getValue());
				return Lat::bot();
			}

			void solve() {
				markExec(fn.getStart());
				for (Node* n : fn)
					if (isValueNode(n))
						ssaWork.push_back(n);
				while (!flowWork.empty() || !ssaWork.empty()) {
					while (!flowWork.empty()) {
						Node* c = flowWork.back();
						flowWork.pop_back();
						visitFlow(c);
					}
					while (!ssaWork.empty()) {
						Node* n = ssaWork.back();
						ssaWork.pop_back();
						visitSSA(n);
					}
				}
			}

			U32 rewrite() {
				U32 changed = 0;
				List<Node*> work;
				for (Node* n : fn)
					work.push_back(n);
				for (Node* n : work) {
					if (isa<ConstantNode>(n) || !isValueNode(n) || !n->hasUsers())
						continue;
					Lat l = get(n);
					if (l.kind != LatKind::Const)
						continue;
					n->replaceAllUsesWith(constant(fn, n->getType(), l.value));
					++changed;
				}
				if (changed)
					fn.eliminateDeadNodes();
				return changed;
			}
		};
	} // namespace detail
	using namespace detail;

	U32 sccp(Function& fn) {
		SCCPSolver solver(fn);
		solver.solve();
		return solver.rewrite();
	}

	const char* SCCPPass::name() const { return "sccp"; }

	U32 SCCPPass::runOnFunction(Function& fn) { return sccp(fn); }
} // namespace rat
