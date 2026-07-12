#include "Pass/Verify.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	VerifyPass::FunctionVerifier::FunctionVerifier(const Function& fn, List<String>& e)
	: fn(fn),
		errs(e),
		startErrs((U32)e.size()) {}

	String VerifyPass::FunctionVerifier::vref(const Node* n) {
		return n ? ("v" + std::to_string(n->getId())) : String("<null>");
	}

	B32 VerifyPass::FunctionVerifier::run() {
		for(Node* n : fn)
			inFn.insert(n);

		check(fn.getStart() != nullptr, nullptr, "function has no Start node");
		check(fn.getStop() != nullptr, nullptr, "function has no Stop node");

		for(Node* n : fn) {
			checkEdges(n);
			checkNode(n);
		}
		checkStopReturns();
		return errs.size() == startErrs;
	}

	void VerifyPass::FunctionVerifier::err(const Node* n, const String& msg) {
		std::ostringstream os;
		if(n)
			os << vref(n) << ": ";
		os << msg;
		errs.push_back(os.str());
	}

	B32 VerifyPass::FunctionVerifier::check(B32 cond, const Node* n, const String& msg) {
		if(!cond)
			err(n, msg);
		return cond;
	}

	B32 VerifyPass::FunctionVerifier::isCtrl(const Node* n) { return n && n->getType()->isControl(); }
	B32 VerifyPass::FunctionVerifier::isMem(const Node* n) { return n && n->getType()->isMemory(); }
	B32 VerifyPass::FunctionVerifier::isData(const Node* n) { return n && n->getType()->isData(); }

	B32 VerifyPass::FunctionVerifier::checkArity(const Node* n) {
		const OpcodeInfo& info = getOpcodeInfo(n->getOpcode());
		U32 c = n->getInputCount();
		U32 lo = (U32)info.minInputs;
		B32 variadic = info.maxInputs < 0;
		if(c < lo || (!variadic && c > (U32)info.maxInputs)) {
			std::ostringstream os;
			os << "expects ";
			if(variadic)
				os << lo << "+";
			else if((U32)info.maxInputs == lo)
				os << lo;
			else
				os << lo << ".." << (U32)info.maxInputs;
			os << " inputs but has " << c;
			err(n, os.str());
			return false;
		}
		return true;
	}

	void VerifyPass::FunctionVerifier::checkEdges(Node* n) {
		auto listsAsUser = [](const Node* def, const Node* user) {
			for(const Node* u : def->getUsers())
				if(u == user)
					return true;
			return false;
		};
		auto listsAsInput = [](const Node* user, const Node* def) {
			for(U32 i = 0, e = user->getInputCount(); i < e; ++i)
				if(user->getInput(i) == def)
					return true;
			return false;
		};

		for(U32 i = 0, e = n->getInputCount(); i < e; ++i) {
			Node* in = n->getInput(i);
			if(!check(in != nullptr, n, "input " + std::to_string(i) + " is null"))
				continue;
			if(!check(inFn.count(in) != 0,
								n,
								"input " + std::to_string(i) + " (" + vref(in) +
										") is not a node of this function"))
				continue;
			check(listsAsUser(in, n),
						n,
						"broken reverse edge: input " + vref(in) + " does not list " + vref(n) + " as a user");
		}
		for(Node* u : n->getUsers()) {
			if(!check(u != nullptr, n, "has a null user"))
				continue;
			check(listsAsInput(u, n),
						n,
						"broken forward edge: user " + vref(u) + " does not use " + vref(n));
		}
	}

	void VerifyPass::FunctionVerifier::checkNode(Node* n) {
		const Type* t = n->getType();
		Opcode op = n->getOpcode();

		if(!checkArity(n))
			return;

		for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
			if(!n->getInput(i))
				return;

		switch(op) {
		case Opcode::Start: {
			check(n == fn.getStart(), n, "duplicate Start node");
			if(!check(t->isTuple(), n, "Start type must be a tuple"))
				break;
			U32 np = fn.getParamCount();
			if(!check(t->getTupleElementCount() == 2 + np,
								n,
								"Start tuple arity does not match (control, memory, params)"))
				break;
			check(t->getTupleElement(0)->isControl(), n, "Start tuple element 0 must be control");
			check(t->getTupleElement(1)->isMemory(), n, "Start tuple element 1 must be memory");
			for(U32 i = 0; i < np; ++i)
				check(t->getTupleElement(2 + i) == fn.getParamType(i),
							n,
							"Start tuple param " + std::to_string(i) + " does not match the function signature");
			break;
		}
		case Opcode::Stop:
			check(n == fn.getStop(), n, "duplicate Stop node");
			check(t->isControl(), n, "Stop type must be control");
			break;

		case Opcode::Return: {
			auto* r = cast<ReturnNode>(n);
			check(isCtrl(r->getControl()), n, "Return input 0 (control) is not control-typed");
			check(isMem(r->getMemory()), n, "Return input 1 (memory) is not memory-typed");
			if(fn.returnsValue()) {
				if(check(r->hasValue(), n, "Return in a value function carries no value"))
					check(r->getValue()->getType() == fn.getReturnType(),
								n,
								"Return value type does not match the function return type");
			} else {
				check(!r->hasValue(), n, "Return in a void function carries a value");
			}
			B32 toStop = false;
			for(Node* u : n->getUsers())
				if(u == fn.getStop())
					toStop = true;
			check(toStop, n, "Return is not connected to the Stop node");
			break;
		}
		case Opcode::Region: {
			auto* r = cast<RegionNode>(n);
			check(t->isControl(), n, "Region type must be control");
			for(U32 i = 0, e = r->getPredecessorCount(); i < e; ++i)
				check(isCtrl(r->getPredecessor(i)),
							n,
							"Region predecessor " + std::to_string(i) + " is not control-typed");
			break;
		}
		case Opcode::If: {
			auto* iff = cast<IfNode>(n);
			check(isCtrl(iff->getControl()), n, "If input 0 (control) is not control-typed");
			check(iff->getPredicate()->getType()->isInt() &&
								iff->getPredicate()->getType()->getIntWidth() == 1,
						n,
						"If predicate must be i1");
			check(t->isTuple() && t->getTupleElementCount() == 2 && t->getTupleElement(0)->isControl() &&
								t->getTupleElement(1)->isControl(),
						n,
						"If type must be (ctrl, ctrl)");
			for(Node* u : n->getUsers())
				if(u->getOpcode() == Opcode::Proj)
					check(cast<ProjNode>(u)->getIndex() <= 1,
								u,
								"projection index out of range for an If (must be 0 or 1)");
			break;
		}
		case Opcode::Proj: {
			auto* p = cast<ProjNode>(n);
			Node* prod = p->getProducer();
			Opcode po = prod->getOpcode();
			if(check(po == Opcode::Start || po == Opcode::If || po == Opcode::Call,
							 n,
							 "Proj producer " + vref(prod) + " is not a multi-output node (Start/If/Call)") &&
				 check(prod->getType()->isTuple(), n, "Proj producer is not tuple-typed") &&
				 check(p->getIndex() < prod->getType()->getTupleElementCount(),
							 n,
							 "Proj index " + std::to_string(p->getIndex()) + " is out of range for " +
									 vref(prod)))
				check(prod->getType()->getTupleElement(p->getIndex()) == t,
							n,
							"Proj type does not match the selected tuple element");
			break;
		}
		case Opcode::Phi: {
			auto* phi = cast<PhiNode>(n);
			Node* reg = phi->getInput(0);
			if(!check(reg && reg->getOpcode() == Opcode::Region, n, "Phi input 0 must be a Region"))
				break;
			auto* r = cast<RegionNode>(reg);
			check(phi->getValueCount() == r->getPredecessorCount(),
						n,
						"Phi has " + std::to_string(phi->getValueCount()) + " values but its region " +
								vref(r) + " has " + std::to_string(r->getPredecessorCount()) + " predecessors");
			check(t->isData() || t->isMemory(), n, "Phi type must be a data or memory type");
			for(U32 i = 0, e = phi->getValueCount(); i < e; ++i)
				check(phi->getValue(i)->getType() == t,
							n,
							"Phi value " + std::to_string(i) + " has a type different from the phi");
			break;
		}
		case Opcode::Constant:
			check(t->isInt(), n, "Constant type must be an integer");
			break;

		case Opcode::Global: {
			auto* g = cast<GlobalNode>(n);
			check(t->isPtr(), n, "Global type must be a pointer");
			check(fn.getModule().getGlobal(g->getSymbol()) != nullptr,
						n,
						"Global references unknown symbol '" + g->getSymbol() + "'");
			break;
		}

		case Opcode::Alloc:
			check(t->isPtr(), n, "Alloc type must be a pointer");
			check(cast<AllocNode>(n)->getAllocType() != nullptr, n, "Alloc has no allocated type");
			break;

		case Opcode::Load: {
			auto* l = cast<LoadNode>(n);
			check(isCtrl(l->getControl()), n, "Load input 0 (control) is not control-typed");
			check(isMem(l->getMemory()), n, "Load input 1 (memory) is not memory-typed");
			check(l->getPointer()->getType()->isPtr(), n, "Load address is not a pointer");
			check(t->isData(), n, "Load result type must be a data type");
			break;
		}
		case Opcode::Store: {
			auto* s = cast<StoreNode>(n);
			check(isCtrl(s->getControl()), n, "Store input 0 (control) is not control-typed");
			check(isMem(s->getMemory()), n, "Store input 1 (memory) is not memory-typed");
			check(s->getPointer()->getType()->isPtr(), n, "Store address is not a pointer");
			check(isData(s->getValue()), n, "Store value is not a data type");
			check(t->isMemory(), n, "Store result type must be memory");
			break;
		}
		case Opcode::Call: {
			auto* c = cast<CallNode>(n);
			check(isCtrl(c->getControl()), n, "Call input 0 (control) is not control-typed");
			check(isMem(c->getMemory()), n, "Call input 1 (memory) is not memory-typed");
			if(!check(t->isTuple(), n, "Call type must be a tuple"))
				break;
			U32 want = c->returnsValue() ? 3 : 2;
			if(!check(
						 t->getTupleElementCount() == want, n, "Call tuple arity does not match returnsValue"))
				break;
			check(t->getTupleElement(0)->isControl(), n, "Call tuple element 0 must be control");
			check(t->getTupleElement(1)->isMemory(), n, "Call tuple element 1 must be memory");
			if(c->returnsValue())
				check(t->getTupleElement(2)->isData(), n, "Call return slot must be a data type");
			break;
		}
		default:
			switch(getOpClass(op)) {
			case OpClass::Binary: {
				auto* b = cast<BinaryNode>(n);
				const Type* lt = b->getLHS()->getType();
				const Type* rt = b->getRHS()->getType();
				check(lt == t, n, "binary result type differs from its left operand");
				B32 shift = op == Opcode::Shl || op == Opcode::LShr || op == Opcode::AShr;
				if(lt->isPtr()) {
					if(check(op == Opcode::Add || op == Opcode::Sub,
									 n,
									 "pointer arithmetic supports only add/sub"))
						check(rt->isInt(), n, "pointer arithmetic offset must be an integer");
				} else if(lt->isInt()) {
					if(shift)
						check(rt->isInt(), n, "shift amount is not an integer");
					else
						check(rt == lt, n, "binary operands have different types");
				} else {
					err(n, "binary operates on a non-data type");
				}
				break;
			}
			case OpClass::Unary: {
				auto* u = cast<UnaryNode>(n);
				check(t->isInt(), n, "unary operates on a non-integer type");
				check(u->getOperand()->getType() == t, n, "unary result type differs from its operand");
				break;
			}
			case OpClass::Compare: {
				auto* c = cast<CompareNode>(n);
				check(t->isInt() && t->getIntWidth() == 1, n, "comparison result must be i1");
				check(c->getLHS()->getType() == c->getRHS()->getType(),
							n,
							"comparison operands have different types");
				break;
			}
			case OpClass::Convert: {
				auto* c = cast<ConvertNode>(n);
				const Type* src = c->getOperand()->getType();
				if(!check(
							 src->isInt() && t->isInt(), n, "conversion requires integer source and destination"))
					break;
				U32 sw = src->getIntWidth(), dw = t->getIntWidth();
				check(!(op == Opcode::Trunc && dw > sw), n, "trunc widens its operand");
				check(!((op == Opcode::SExt || op == Opcode::ZExt) && dw < sw),
							n,
							"extension narrows its operand");
				break;
			}
			case OpClass::None:
				break;
			}
			break;
		}
	}

	void VerifyPass::FunctionVerifier::checkStopReturns() {
		Node* stop = fn.getStop();
		if(!stop)
			return;
		for(U32 i = 0, e = stop->getInputCount(); i < e; ++i)
			check(!stop->getInput(i) || stop->getInput(i)->getOpcode() == Opcode::Return,
						stop,
						"Stop input " + std::to_string(i) + " (" + vref(stop->getInput(i)) +
								") is not a Return");
		check(stop->getInputCount() != 0, stop, "function never returns (Stop has no Return inputs)");
	}

	B32 verify(const Function& fn, List<String>& errors) {
		return VerifyPass::FunctionVerifier(fn, errors).run();
	}

	B32 verify(const Module& module, List<String>& errors) {
		B32 ok = true;
		for(const Function* fn : module) {
			List<String> local;
			if(!verify(*fn, local)) {
				ok = false;
				for(String& s : local)
					errors.push_back(fn->getName() + ": " + s);
			}
		}
		return ok;
	}

	B32 verify(const Function& fn, std::ostream& os) {
		List<String> errors;
		B32 ok = verify(fn, errors);
		for(const String& s : errors)
			os << s << "\n";
		return ok;
	}

	B32 verify(const Module& module, std::ostream& os) {
		List<String> errors;
		B32 ok = verify(module, errors);
		for(const String& s : errors)
			os << s << "\n";
		return ok;
	}

	VerifyPass::VerifyPass(std::ostream& os)
	: os(&os) {}

	const C8* VerifyPass::name() const { return "verify"; }

	B32 VerifyPass::run(Module& module) {
		verify(module, *os);
		return false;
	}
} // namespace rat
