#include "Pass/Verify.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	namespace detail {
		String vref(const Node* n) { return n ? ("v" + std::to_string(n->getId())) : String("<null>"); }

		struct FunctionVerifier {
			const Function& fn;
			List<String>& errs;
			Set<const Node*> inFn;
			U32 startErrs;

			FunctionVerifier(const Function& fn, List<String>& e)
			: fn(fn),
				errs(e),
				startErrs((U32)e.size()) {}

			B32 run() {
				for (Node* n : fn)
					inFn.insert(n);

				if (!fn.getStart())
					err(nullptr, "function has no Start node");
				if (!fn.getStop())
					err(nullptr, "function has no Stop node");

				for (Node* n : fn) {
					checkEdges(n);
					checkNode(n);
				}
				checkStopReturns();
				return errs.size() == startErrs;
			}

			void err(const Node* n, const String& msg) {
				std::ostringstream os;
				if (n)
					os << vref(n) << ": ";
				os << msg;
				errs.push_back(os.str());
			}

			static B32 isCtrl(const Node* n) { return n->getType()->isControl(); }
			static B32 isMem(const Node* n) { return n->getType()->isMemory(); }
			static B32 isData(const Node* n) { return n->getType()->isData(); }

			B32 checkArity(const Node* n) {
				const OpcodeInfo& info = getOpcodeInfo(n->getOpcode());
				U32 c = n->getInputCount();
				U32 lo = (U32)info.minInputs;
				B32 variadic = info.maxInputs < 0;
				if (c < lo || (!variadic && c > (U32)info.maxInputs)) {
					std::ostringstream os;
					os << "expects ";
					if (variadic)
						os << lo << "+";
					else if ((U32)info.maxInputs == lo)
						os << lo;
					else
						os << lo << ".." << (U32)info.maxInputs;
					os << " inputs but has " << c;
					err(n, os.str());
					return false;
				}
				return true;
			}

			void checkEdges(Node* n) {
				for (U32 i = 0, e = n->getInputCount(); i < e; ++i) {
					Node* in = n->getInput(i);
					if (!in) {
						err(n, "input " + std::to_string(i) + " is null");
						continue;
					}
					if (!inFn.count(in)) {
						err(n,
								"input " + std::to_string(i) + " (" + vref(in) +
										") is not a node of this function");
						continue;
					}
					B32 found = false;
					for (Node* u : in->getUsers())
						if (u == n) {
							found = true;
							break;
						}
					if (!found)
						err(n,
								"broken reverse edge: input " + vref(in) + " does not list " + vref(n) +
										" as a user");
				}
				for (Node* u : n->getUsers()) {
					if (!u) {
						err(n, "has a null user");
						continue;
					}
					B32 found = false;
					for (U32 i = 0, e = u->getInputCount(); i < e; ++i)
						if (u->getInput(i) == n) {
							found = true;
							break;
						}
					if (!found)
						err(n, "broken forward edge: user " + vref(u) + " does not use " + vref(n));
				}
			}

			void checkNode(Node* n) {
				const Type* t = n->getType();
				Opcode op = n->getOpcode();

				if (!checkArity(n))
					return;

				switch (op) {
				case Opcode::Start: {
					if (n != fn.getStart())
						err(n, "duplicate Start node");
					if (!t->isTuple()) {
						err(n, "Start type must be a tuple");
						break;
					}
					U32 np = fn.getParamCount();
					if (t->getTupleElementCount() != 2 + np)
						err(n, "Start tuple arity does not match (control, memory, params)");
					else {
						if (!t->getTupleElement(0)->isControl())
							err(n, "Start tuple element 0 must be control");
						if (!t->getTupleElement(1)->isMemory())
							err(n, "Start tuple element 1 must be memory");
						for (U32 i = 0; i < np; ++i)
							if (t->getTupleElement(2 + i) != fn.getParamType(i))
								err(n,
										"Start tuple param " + std::to_string(i) +
												" does not match the function signature");
					}
					break;
				}
				case Opcode::Stop:
					if (n != fn.getStop())
						err(n, "duplicate Stop node");
					if (!t->isControl())
						err(n, "Stop type must be control");
					break;

				case Opcode::Return: {
					auto* r = cast<ReturnNode>(n);
					if (!isCtrl(r->getControl()))
						err(n, "Return input 0 (control) is not control-typed");
					if (!isMem(r->getMemory()))
						err(n, "Return input 1 (memory) is not memory-typed");
					if (fn.returnsValue()) {
						if (!r->hasValue())
							err(n, "Return in a value function carries no value");
						else if (r->getValue()->getType() != fn.getReturnType())
							err(n, "Return value type does not match the function return type");
					} else if (r->hasValue()) {
						err(n, "Return in a void function carries a value");
					}
					B32 toStop = false;
					for (Node* u : n->getUsers())
						if (u == fn.getStop())
							toStop = true;
					if (!toStop)
						err(n, "Return is not connected to the Stop node");
					break;
				}
				case Opcode::Region: {
					auto* r = cast<RegionNode>(n);
					if (!t->isControl())
						err(n, "Region type must be control");
					for (U32 i = 0, e = r->getPredecessorCount(); i < e; ++i)
						if (!isCtrl(r->getPredecessor(i)))
							err(n, "Region predecessor " + std::to_string(i) + " is not control-typed");
					break;
				}
				case Opcode::If: {
					auto* iff = cast<IfNode>(n);
					if (!isCtrl(iff->getControl()))
						err(n, "If input 0 (control) is not control-typed");
					if (!iff->getPredicate()->getType()->isInt() ||
							iff->getPredicate()->getType()->getIntWidth() != 1)
						err(n, "If predicate must be i1");
					if (!t->isTuple() || t->getTupleElementCount() != 2 ||
							!t->getTupleElement(0)->isControl() || !t->getTupleElement(1)->isControl())
						err(n, "If type must be (ctrl, ctrl)");
					for (Node* u : n->getUsers())
						if (u->getOpcode() == Opcode::Proj)
							if (cast<ProjNode>(u)->getIndex() > 1)
								err(u, "projection index out of range for an If (must be 0 or 1)");
					break;
				}
				case Opcode::Proj: {
					auto* p = cast<ProjNode>(n);
					Node* prod = p->getProducer();
					Opcode po = prod->getOpcode();
					if (po != Opcode::Start && po != Opcode::If && po != Opcode::Call)
						err(n, "Proj producer " + vref(prod) + " is not a multi-output node (Start/If/Call)");
					else if (!prod->getType()->isTuple())
						err(n, "Proj producer is not tuple-typed");
					else if (p->getIndex() >= prod->getType()->getTupleElementCount())
						err(n,
								"Proj index " + std::to_string(p->getIndex()) + " is out of range for " +
										vref(prod));
					else if (prod->getType()->getTupleElement(p->getIndex()) != t)
						err(n, "Proj type does not match the selected tuple element");
					break;
				}
				case Opcode::Phi: {
					auto* phi = cast<PhiNode>(n);
					Node* reg = phi->getInput(0);
					if (!reg || reg->getOpcode() != Opcode::Region) {
						err(n, "Phi input 0 must be a Region");
						break;
					}
					auto* r = cast<RegionNode>(reg);
					if (phi->getValueCount() != r->getPredecessorCount())
						err(n,
								"Phi has " + std::to_string(phi->getValueCount()) + " values but its region " +
										vref(r) + " has " + std::to_string(r->getPredecessorCount()) + " predecessors");
					if (!t->isData() && !t->isMemory())
						err(n, "Phi type must be a data or memory type");
					for (U32 i = 0, e = phi->getValueCount(); i < e; ++i)
						if (phi->getValue(i)->getType() != t)
							err(n, "Phi value " + std::to_string(i) + " has a type different from the phi");
					break;
				}
				case Opcode::Constant:
					if (!t->isInt())
						err(n, "Constant type must be an integer");
					break;

				case Opcode::Global: {
					auto* g = cast<GlobalNode>(n);
					if (!t->isPtr())
						err(n, "Global type must be a pointer");
					if (!fn.getModule().getGlobal(g->getSymbol()))
						err(n, "Global references unknown symbol '" + g->getSymbol() + "'");
					break;
				}

				case Opcode::Alloc:
					if (!t->isPtr())
						err(n, "Alloc type must be a pointer");
					if (!cast<AllocNode>(n)->getAllocType())
						err(n, "Alloc has no allocated type");
					break;

				case Opcode::Load: {
					auto* l = cast<LoadNode>(n);
					if (!isCtrl(l->getControl()))
						err(n, "Load input 0 (control) is not control-typed");
					if (!isMem(l->getMemory()))
						err(n, "Load input 1 (memory) is not memory-typed");
					if (!l->getPointer()->getType()->isPtr())
						err(n, "Load address is not a pointer");
					if (!t->isData())
						err(n, "Load result type must be a data type");
					break;
				}
				case Opcode::Store: {
					auto* s = cast<StoreNode>(n);
					if (!isCtrl(s->getControl()))
						err(n, "Store input 0 (control) is not control-typed");
					if (!isMem(s->getMemory()))
						err(n, "Store input 1 (memory) is not memory-typed");
					if (!s->getPointer()->getType()->isPtr())
						err(n, "Store address is not a pointer");
					if (!isData(s->getValue()))
						err(n, "Store value is not a data type");
					if (!t->isMemory())
						err(n, "Store result type must be memory");
					break;
				}
				case Opcode::Call: {
					auto* c = cast<CallNode>(n);
					if (!isCtrl(c->getControl()))
						err(n, "Call input 0 (control) is not control-typed");
					if (!isMem(c->getMemory()))
						err(n, "Call input 1 (memory) is not memory-typed");
					if (!t->isTuple()) {
						err(n, "Call type must be a tuple");
						break;
					}
					U32 want = c->returnsValue() ? 3 : 2;
					if (t->getTupleElementCount() != want)
						err(n, "Call tuple arity does not match returnsValue");
					else {
						if (!t->getTupleElement(0)->isControl())
							err(n, "Call tuple element 0 must be control");
						if (!t->getTupleElement(1)->isMemory())
							err(n, "Call tuple element 1 must be memory");
						if (c->returnsValue() && !t->getTupleElement(2)->isData())
							err(n, "Call return slot must be a data type");
					}
					break;
				}
				default:
					switch (getOpClass(op)) {
					case OpClass::Binary: {
						auto* b = cast<BinaryNode>(n);
						const Type* lt = b->getLHS()->getType();
						const Type* rt = b->getRHS()->getType();
						if (lt != t)
							err(n, "binary result type differs from its left operand");
						B32 shift = op == Opcode::Shl || op == Opcode::LShr || op == Opcode::AShr;
						if (lt->isPtr()) {
							if (op != Opcode::Add && op != Opcode::Sub)
								err(n, "pointer arithmetic supports only add/sub");
							else if (!rt->isInt())
								err(n, "pointer arithmetic offset must be an integer");
						} else if (lt->isInt()) {
							if (shift) {
								if (!rt->isInt())
									err(n, "shift amount is not an integer");
							} else if (rt != lt) {
								err(n, "binary operands have different types");
							}
						} else {
							err(n, "binary operates on a non-data type");
						}
						break;
					}
					case OpClass::Unary: {
						auto* u = cast<UnaryNode>(n);
						if (!t->isInt())
							err(n, "unary operates on a non-integer type");
						if (u->getOperand()->getType() != t)
							err(n, "unary result type differs from its operand");
						break;
					}
					case OpClass::Compare: {
						auto* c = cast<CompareNode>(n);
						if (!t->isInt() || t->getIntWidth() != 1)
							err(n, "comparison result must be i1");
						if (c->getLHS()->getType() != c->getRHS()->getType())
							err(n, "comparison operands have different types");
						break;
					}
					case OpClass::Convert: {
						auto* c = cast<ConvertNode>(n);
						const Type* s = c->getOperand()->getType();
						if (!s->isInt() || !t->isInt()) {
							err(n, "conversion requires integer source and destination");
							break;
						}
						U32 sw = s->getIntWidth(), dw = t->getIntWidth();
						if (op == Opcode::Trunc && dw > sw)
							err(n, "trunc widens its operand");
						if ((op == Opcode::SExt || op == Opcode::ZExt) && dw < sw)
							err(n, "extension narrows its operand");
						break;
					}
					case OpClass::None:
						break;
					}
					break;
				}
			}

			void checkStopReturns() {
				if (!fn.getStop())
					return;
				Node* stop = fn.getStop();
				for (U32 i = 0, e = stop->getInputCount(); i < e; ++i)
					if (stop->getInput(i) && stop->getInput(i)->getOpcode() != Opcode::Return)
						err(stop,
								"Stop input " + std::to_string(i) + " (" + vref(stop->getInput(i)) +
										") is not a Return");
				if (stop->getInputCount() == 0)
					err(stop, "function never returns (Stop has no Return inputs)");
			}
		};
	} // namespace detail
	using namespace detail;

	B32 verify(const Function& fn, List<String>& errors) {
		return FunctionVerifier(fn, errors).run();
	}

	B32 verify(const Module& module, List<String>& errors) {
		B32 ok = true;
		for (const Function* fn : module) {
			List<String> local;
			if (!verify(*fn, local)) {
				ok = false;
				for (String& s : local)
					errors.push_back(fn->getName() + ": " + s);
			}
		}
		return ok;
	}

	B32 verify(const Function& fn, std::ostream& os) {
		List<String> errors;
		B32 ok = verify(fn, errors);
		for (const String& s : errors)
			os << s << "\n";
		return ok;
	}

	B32 verify(const Module& module, std::ostream& os) {
		List<String> errors;
		B32 ok = verify(module, errors);
		for (const String& s : errors)
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
