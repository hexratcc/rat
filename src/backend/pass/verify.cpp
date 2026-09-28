#include "pass/verify.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"
#include "ir/type.h"

#include <sstream>

namespace rat {
	VerifyPass::FunctionVerifier::FunctionVerifier(const Function& fn, std::ostream& os)
	: fn(fn),
		os(os) {}

	String VerifyPass::FunctionVerifier::vref(const Node* n) {
		return n ? ("v" + std::to_string(n->getId())) : String("<null>");
	}

	void VerifyPass::FunctionVerifier::run() {
		for(Node* n : fn)
			inFn.insert(n);

		if(!fn.getStart())
			err(nullptr, "function has no Start node");
		if(!fn.getStop())
			err(nullptr, "function has no Stop node");

		for(Node* n : fn) {
			checkEdges(n);
			checkNode(n);
		}
		checkStopReturns();
	}

	void VerifyPass::FunctionVerifier::err(const Node* n, const String& msg) {
		os << fn.getName() << ": ";
		if(n)
			os << vref(n) << ": ";
		os << msg << "\n";
	}

	B32 VerifyPass::FunctionVerifier::isCtrl(const Node* n) { return n && n->getType()->isControl(); }
	B32 VerifyPass::FunctionVerifier::isMem(const Node* n) { return n && n->getType()->isMemory(); }
	B32 VerifyPass::FunctionVerifier::isData(const Node* n) { return n && n->getType()->isData(); }
	B32 VerifyPass::FunctionVerifier::isBool(const Type* t) {
		return t->isInt() && t->getIntWidth() == 1;
	}

	B32 VerifyPass::FunctionVerifier::listsAsUser(const Node* def, const Node* user) {
		for(const Node* u : def->getUsers())
			if(u == user)
				return true;
		return false;
	}

	B32 VerifyPass::FunctionVerifier::listsAsInput(const Node* user, const Node* def) {
		for(U32 i = 0, e = user->getInputCount(); i < e; ++i)
			if(user->getInput(i) == def)
				return true;
		return false;
	}

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
		for(U32 i = 0, e = n->getInputCount(); i < e; ++i) {
			Node* in = n->getInput(i);
			if(!in) {
				err(n, "input " + std::to_string(i) + " is null");
				continue;
			}
			if(!inFn.count(in)) {
				err(n, "input " + std::to_string(i) + " (" + vref(in) + ") is not a node of this function");
				continue;
			}
			if(!listsAsUser(in, n))
				err(n,
						"broken reverse edge: input " + vref(in) + " does not list " + vref(n) + " as a user");
		}
		for(Node* u : n->getUsers()) {
			if(!u) {
				err(n, "has a null user");
				continue;
			}
			if(!listsAsInput(u, n))
				err(n, "broken forward edge: user " + vref(u) + " does not use " + vref(n));
		}
	}

	void VerifyPass::FunctionVerifier::checkNode(Node* n) {
		if(!checkArity(n))
			return;

		for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
			if(!n->getInput(i))
				return;

		const Type* t = n->getType();
		switch(n->getOpcode()) {
		case Opcode::Start:
			checkStart(n);
			break;
		case Opcode::Stop:
			if(n != fn.getStop())
				err(n, "duplicate Stop node");
			if(!t->isControl())
				err(n, "Stop type must be control");
			break;
		case Opcode::Return:
			checkReturn(n);
			break;
		case Opcode::Region:
			checkRegion(n);
			break;
		case Opcode::If:
			checkIf(n);
			break;
		case Opcode::Switch:
			checkSwitch(n);
			break;
		case Opcode::Proj:
			checkProj(n);
			break;
		case Opcode::Phi:
			checkPhi(n);
			break;
		case Opcode::Constant:
			if(!(t->isInt() || t->isFloat()))
				err(n, "Constant type must be an integer or a float");
			break;
		case Opcode::Global:
			if(!t->isPtr())
				err(n, "Global type must be a pointer");
			if(!fn.getModule().getGlobal(cast<GlobalNode>(n)->getSymbol()))
				err(n, "Global references unknown symbol '" + cast<GlobalNode>(n)->getSymbol() + "'");
			break;
		case Opcode::Alloc:
			if(!t->isPtr())
				err(n, "Alloc type must be a pointer");
			if(!cast<AllocNode>(n)->getAllocType())
				err(n, "Alloc has no allocated type");
			break;
		case Opcode::StackAlloc:
		case Opcode::StackSave:
			if(!t->isPtr())
				err(n, "stack op type must be a pointer");
			break;
		case Opcode::StackRestore:
			if(!t->isMemory())
				err(n, "StackRestore type must be memory");
			break;
		case Opcode::Splat:
		case Opcode::Extract:
		case Opcode::Pack:
		case Opcode::Shuffle:
			checkLaneOp(n);
			break;
		case Opcode::Select:
			checkSelect(n);
			break;
		case Opcode::Load:
		case Opcode::Store:
			checkMemoryOp(n);
			break;
		case Opcode::Call:
			checkCall(n);
			break;
		case Opcode::Asm:
			checkAsm(n);
			break;
		default:
			checkArithmetic(n);
			break;
		}
	}

	void VerifyPass::FunctionVerifier::checkCtrlMem(const Node* n, const C8* what) {
		if(!isCtrl(n->getInput(0)))
			err(n, String(what) + " input 0 (control) is not control-typed");
		if(!isMem(n->getInput(1)))
			err(n, String(what) + " input 1 (memory) is not memory-typed");
	}

	B32 VerifyPass::FunctionVerifier::checkTuple(const Node* n, const C8* op, U32 len, const C8* by) {
		const Type* t = n->getType();
		if(!t->isTuple()) {
			err(n, String(op) + " type must be a tuple");
			return false;
		}
		if(t->getTupleElementCount() != len) {
			err(n, String(op) + " tuple arity does not match " + by);
			return false;
		}
		if(!t->getTupleElement(0)->isControl())
			err(n, String(op) + " tuple element 0 must be control");
		if(!t->getTupleElement(1)->isMemory())
			err(n, String(op) + " tuple element 1 must be memory");
		return true;
	}

	void VerifyPass::FunctionVerifier::checkStart(const Node* n) {
		if(n != fn.getStart())
			err(n, "duplicate Start node");
		U32 np = fn.getParamCount();
		if(!checkTuple(n, "Start", 2 + np, "(control, memory, params)"))
			return;
		for(U32 i = 0; i < np; ++i)
			if(n->getType()->getTupleElement(2 + i) != fn.getParamType(i))
				err(n, "Start tuple param " + std::to_string(i) + " does not match the function signature");
	}

	void VerifyPass::FunctionVerifier::checkReturn(const Node* n) {
		const ReturnNode* r = cast<ReturnNode>(n);
		checkCtrlMem(n, "Return");
		if(fn.returnsValue()) {
			if(!r->hasValue())
				err(n, "Return in a value function carries no value");
			else if(r->getValue()->getType() != fn.getReturnType())
				err(n, "Return value type does not match the function return type");
		} else if(r->hasValue()) {
			err(n, "Return in a void function carries a value");
		}
		if(!listsAsUser(n, fn.getStop()))
			err(n, "Return is not connected to the Stop node");
	}

	void VerifyPass::FunctionVerifier::checkRegion(const Node* n) {
		const RegionNode* r = cast<RegionNode>(n);
		if(!n->getType()->isControl())
			err(n, "Region type must be control");
		for(U32 i = 0, e = r->getPredecessorCount(); i < e; ++i)
			if(!isCtrl(r->getPredecessor(i)))
				err(n, "Region predecessor " + std::to_string(i) + " is not control-typed");
	}

	void VerifyPass::FunctionVerifier::checkIf(const Node* n) {
		const IfNode* iff = cast<IfNode>(n);
		const Type* t = n->getType();
		if(!isCtrl(iff->getControl()))
			err(n, "If input 0 (control) is not control-typed");
		if(!isBool(iff->getPredicate()->getType()))
			err(n, "If predicate must be i1");
		if(!(t->isTuple() && t->getTupleElementCount() == 2 && t->getTupleElement(0)->isControl() &&
				 t->getTupleElement(1)->isControl()))
			err(n, "If type must be (ctrl, ctrl)");
		for(Node* u : n->getUsers())
			if(u->getOpcode() == Opcode::Proj && cast<ProjNode>(u)->getIndex() > 1)
				err(u, "projection index out of range for an If (must be 0 or 1)");
	}

	void VerifyPass::FunctionVerifier::checkSwitch(const Node* n) {
		const SwitchNode* sw = cast<SwitchNode>(n);
		const Type* t = n->getType();
		if(!isCtrl(sw->getControl()))
			err(n, "Switch input 0 (control) is not control-typed");
		if(!sw->getSelector()->getType()->isInt())
			err(n, "Switch selector must be an integer");
		if(!t->isTuple() || t->getTupleElementCount() == 0) {
			err(n, "Switch type must be a non-empty tuple of control");
			return;
		}
		for(U32 i = 0, e = t->getTupleElementCount(); i < e; ++i)
			if(!t->getTupleElement(i)->isControl())
				err(n, "Switch tuple element " + std::to_string(i) + " must be control");
	}

	void VerifyPass::FunctionVerifier::checkProj(const Node* n) {
		const ProjNode* p = cast<ProjNode>(n);
		Node* prod = p->getProducer();
		Opcode po = prod->getOpcode();
		if(!(po == Opcode::Start || po == Opcode::If || po == Opcode::Call || po == Opcode::Switch ||
				 po == Opcode::Asm)) {
			err(n,
					"Proj producer " + vref(prod) + " is not a multi-output node (Start/If/Call/Switch/Asm)");
		} else if(!prod->getType()->isTuple()) {
			err(n, "Proj producer is not tuple-typed");
		} else if(p->getIndex() >= prod->getType()->getTupleElementCount()) {
			err(n, "Proj index " + std::to_string(p->getIndex()) + " is out of range for " + vref(prod));
		} else if(prod->getType()->getTupleElement(p->getIndex()) != n->getType()) {
			err(n, "Proj type does not match the selected tuple element");
		}
	}

	void VerifyPass::FunctionVerifier::checkPhi(const Node* n) {
		const PhiNode* phi = cast<PhiNode>(n);
		const Type* t = n->getType();
		Node* reg = phi->getInput(0);
		if(reg->getOpcode() != Opcode::Region) {
			err(n, "Phi input 0 must be a Region");
			return;
		}
		const RegionNode* r = cast<RegionNode>(reg);
		if(phi->getValueCount() != r->getPredecessorCount())
			err(n,
					"Phi has " + std::to_string(phi->getValueCount()) + " values but its region " + vref(r) +
							" has " + std::to_string(r->getPredecessorCount()) + " predecessors");
		if(!(t->isData() || t->isMemory()))
			err(n, "Phi type must be a data or memory type");
		for(U32 i = 0, e = phi->getValueCount(); i < e; ++i)
			if(phi->getValue(i)->getType() != t)
				err(n, "Phi value " + std::to_string(i) + " has a type different from the phi");
	}

	void VerifyPass::FunctionVerifier::checkLaneOp(const Node* n) {
		const Type* t = n->getType();
		const Type* in = n->getInput(0)->getType();
		switch(n->getOpcode()) {
		case Opcode::Splat:
			if(!t->isVec())
				err(n, "Splat type must be a vector");
			else if(in != t->getVecElement())
				err(n, "Splat scalar type does not match the vector element");
			break;
		case Opcode::Extract:
			if(!in->isVec()) {
				err(n, "Extract operand must be a vector");
				break;
			}
			if(t != in->getVecElement())
				err(n, "Extract type does not match the vector element");
			if(cast<ExtractNode>(n)->getLane() >= in->getVecLanes())
				err(n, "Extract lane is out of range");
			break;
		case Opcode::Pack:
			if(!t->isVec()) {
				err(n, "Pack type must be a vector");
				break;
			}
			if(n->getInputCount() != t->getVecLanes())
				err(n, "Pack operand count does not match the lane count");
			for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
				if(n->getInput(i)->getType() != t->getVecElement())
					err(n, "Pack lane " + std::to_string(i) + " type does not match the vector element");
			break;
		default:
			if(!t->isVec())
				err(n, "Shuffle type must be a vector");
			else if(in != t)
				err(n, "Shuffle operand type does not match the result vector");
			break;
		}
	}

	void VerifyPass::FunctionVerifier::checkSelect(const Node* n) {
		const SelectNode* s = cast<SelectNode>(n);
		const Type* t = n->getType();
		if(!t->isInt() && !t->isPtr())
			err(n, "Select type must be an integer or pointer");
		if(!isBool(s->getCondition()->getType()))
			err(n, "Select condition must be i1");
		if(s->getTrue()->getType() != t)
			err(n, "Select then-value type does not match the result");
		if(s->getFalse()->getType() != t)
			err(n, "Select else-value type does not match the result");
	}

	void VerifyPass::FunctionVerifier::checkMemoryOp(const Node* n) {
		B32 load = n->getOpcode() == Opcode::Load;
		const C8* what = load ? "Load" : "Store";
		checkCtrlMem(n, what);
		if(!n->getInput(2)->getType()->isPtr())
			err(n, String(what) + " address is not a pointer");
		if(load) {
			if(!n->getType()->isData())
				err(n, "Load result type must be a data type");
			return;
		}
		if(!isData(n->getInput(3)))
			err(n, "Store value is not a data type");
		if(!n->getType()->isMemory())
			err(n, "Store result type must be memory");
	}

	void VerifyPass::FunctionVerifier::checkCall(const Node* n) {
		const CallNode* c = cast<CallNode>(n);
		checkCtrlMem(n, "Call");
		U32 want = c->returnsValue() ? 3 : 2;
		if(!checkTuple(n, "Call", want, "returnsValue"))
			return;
		if(c->returnsValue() && !n->getType()->getTupleElement(2)->isData())
			err(n, "Call return slot must be a data type");
	}

	void VerifyPass::FunctionVerifier::checkAsm(const Node* n) {
		const AsmNode* a = cast<AsmNode>(n);
		checkCtrlMem(n, "Asm");
		if(!checkTuple(n, "Asm", 2 + a->getOutputCount(), "the output count"))
			return;
		for(U32 i = 0; i < a->getOutputCount(); ++i)
			if(!n->getType()->getTupleElement(2 + i)->isData())
				err(n, "Asm output slot must be a data type");
	}

	void VerifyPass::FunctionVerifier::checkArithmetic(const Node* n) {
		switch(getOpClass(n->getOpcode())) {
		case OpClass::Binary:
			checkBinary(n);
			break;
		case OpClass::Unary:
			checkUnary(n);
			break;
		case OpClass::Compare:
			if(!isBool(n->getType()))
				err(n, "comparison result must be i1");
			if(n->getInput(0)->getType() != n->getInput(1)->getType())
				err(n, "comparison operands have different types");
			break;
		case OpClass::Convert:
			checkConvert(n);
			break;
		case OpClass::None:
			break;
		}
	}

	void VerifyPass::FunctionVerifier::checkBinary(const Node* n) {
		Opcode op = n->getOpcode();
		const Type* lt = n->getInput(0)->getType();
		const Type* rt = n->getInput(1)->getType();
		if(lt != n->getType())
			err(n, "binary result type differs from its left operand");
		B32 shift = op == Opcode::Shl || op == Opcode::LShr || op == Opcode::AShr ||
								op == Opcode::Rotl || op == Opcode::Rotr;
		if(lt->isPtr()) {
			if(!(op == Opcode::Add || op == Opcode::Sub))
				err(n, "pointer arithmetic supports only add/sub");
			else if(!rt->isInt())
				err(n, "pointer arithmetic offset must be an integer");
		} else if(lt->isInt()) {
			if(shift) {
				if(!rt->isInt())
					err(n, "shift amount is not an integer");
			} else if(rt != lt) {
				err(n, "binary operands have different types");
			}
		} else if(lt->isVec()) {
			if(rt != lt)
				err(n, "binary operands have different types");
			// only the SSE2-lowerable subset may appear at vector types
			const Type* et = lt->getVecElement();
			B32 legal = et->isInt() ? (op == Opcode::Add || op == Opcode::Sub || op == Opcode::And ||
																 op == Opcode::Or || op == Opcode::Xor)
															: (op == Opcode::FAdd || op == Opcode::FSub || op == Opcode::FMul ||
																 op == Opcode::FDiv);
			if(!legal)
				err(n, "binary opcode has no vector lowering");
		} else if(lt->isFloat()) {
			if(rt != lt)
				err(n, "binary operands have different types");
		} else {
			err(n, "binary operates on a non-data type");
		}
	}

	void VerifyPass::FunctionVerifier::checkUnary(const Node* n) {
		auto* u = cast<UnaryNode>(n);
		const Type* t = n->getType();
		if(n->getOpcode() == Opcode::FNeg) {
			if(!t->isFloat())
				err(n, "fneg operates on a non-float type");
		} else if(!t->isInt()) {
			err(n, "unary operates on a non-integer type");
		}
		if(u->getOperand()->getType() != t)
			err(n, "unary result type differs from its operand");
	}

	void VerifyPass::FunctionVerifier::checkConvert(const Node* n) {
		auto* c = cast<ConvertNode>(n);
		Opcode op = n->getOpcode();
		const Type* t = n->getType();
		const Type* src = c->getOperand()->getType();
		switch(op) {
		case Opcode::Trunc:
		case Opcode::SExt:
		case Opcode::ZExt: {
			if(!(src->isInt() && t->isInt())) {
				err(n, "conversion requires integer source and destination");
				break;
			}
			U32 sw = src->getIntWidth(), dw = t->getIntWidth();
			if(op == Opcode::Trunc && dw > sw)
				err(n, "trunc widens its operand");
			if((op == Opcode::SExt || op == Opcode::ZExt) && dw < sw)
				err(n, "extension narrows its operand");
			break;
		}
		case Opcode::SIToFP:
		case Opcode::UIToFP:
			if(!(src->isInt() && t->isFloat()))
				err(n, "conversion requires an integer source and a float destination");
			break;
		case Opcode::FPToSI:
		case Opcode::FPToUI:
			if(!(src->isFloat() && t->isInt()))
				err(n, "conversion requires a float source and an integer destination");
			break;
		case Opcode::FPExt:
		case Opcode::FPTrunc: {
			if(!(src->isFloat() && t->isFloat())) {
				err(n, "conversion requires float source and destination");
				break;
			}
			U32 sw = src->getFloatWidth(), dw = t->getFloatWidth();
			if(op == Opcode::FPTrunc && dw > sw)
				err(n, "fptrunc widens its operand");
			if(op == Opcode::FPExt && dw < sw)
				err(n, "fpext narrows its operand");
			break;
		}
		default:
			break;
		}
	}

	void VerifyPass::FunctionVerifier::checkStopReturns() {
		Node* stop = fn.getStop();
		if(!stop)
			return;
		for(U32 i = 0, e = stop->getInputCount(); i < e; ++i)
			if(stop->getInput(i) && stop->getInput(i)->getOpcode() != Opcode::Return)
				err(stop,
						"Stop input " + std::to_string(i) + " (" + vref(stop->getInput(i)) +
								") is not a Return");
		if(stop->getInputCount() == 0)
			err(stop, "function never returns (Stop has no Return inputs)");
	}

	VerifyPass::VerifyPass(std::ostream& os)
	: os(&os) {}

	const C8* VerifyPass::name() const { return "verify"; }

	B32 VerifyPass::run(Module& module, const TargetInfo&) {
		for(const Function* fn : module)
			FunctionVerifier(*fn, *os).run();
		return false;
	}
} // namespace rat
