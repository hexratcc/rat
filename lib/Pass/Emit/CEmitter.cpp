#include "Pass/Emit/CEmitter.h"

#include "CodeGen/Schedule.h"
#include "CodeGen/Target.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

#include <sstream>

namespace rat {
	namespace {
		String intCType(U32 width, B32 isSigned) {
			if (width <= 1)
				return "int"; // booleans
			const char* s;
			if (width <= 8)
				s = isSigned ? "int8_t" : "uint8_t";
			else if (width <= 16)
				s = isSigned ? "int16_t" : "uint16_t";
			else if (width <= 32)
				s = isSigned ? "int32_t" : "uint32_t";
			else
				s = isSigned ? "int64_t" : "uint64_t";
			// TODO: bigger than 64b
			return s;
		}

		String cType(const Type* t, B32 isSigned = true) {
			if (t->isInt())
				return intCType(t->getIntWidth(), isSigned);
			if (t->isPtr())
				return "char *"; // byte-addressed; typed accesses cast
			return "void";
		}

		void printSignature(const Function& fn, std::ostream& os) {
			os << (fn.returnsValue() ? cType(fn.getReturnType()) : String("void"))
				 << " " << fn.getName() << "(";
			if (fn.getParamCount() == 0)
				os << "void";
			for (U32 i = 0, e = fn.getParamCount(); i < e; ++i) {
				if (i)
					os << ", ";
				os << cType(fn.getParamType(i)) << " arg" << i;
			}
			os << ")";
		}

		struct CEmitter {
			const Function& fn;
			std::ostream& os;
			Schedule sched;
			Set<const Node*> needTemp;

			using TK = Schedule::TermKind;

			CEmitter(const Function& fn, std::ostream& os)
					: fn(fn), os(os), sched(fn) {}

			void run() {
				computeNeedTemp();
				emit();
			}

			static B32 producesTemp(const Node* n) {
				return Schedule::isFloating(n) || n->getOpcode() == Opcode::Load;
			}

			void computeNeedTemp() {
				for (I32 b = 0; b < sched.numBlocks(); ++b) {
					for (PhiNode* phi : sched.block(b).phis)
						needTemp.insert(phi); // data phis only
					for (Node* n : sched.block(b).nodes) {
						if (producesTemp(n))
							needTemp.insert(n);
						if (n->getOpcode() == Opcode::Call)
							if (ProjNode* p = n->projection(CallNode::valueProjIndex()))
								needTemp.insert(p);
					}
				}
			}

			String temp(const Node* n) const {
				return "t" + std::to_string(n->getId());
			}

			String valueExpr(Node* n) {
				if (n->getOpcode() == Opcode::Constant) {
					U32 width = n->getType()->getIntWidth();
					std::ostringstream oss;
					oss << cast<ConstantNode>(n)->getValue();
					if (width > 32)
						oss << "LL";
					return oss.str();
				}
				if (const ProjNode* p = dyn_cast<ProjNode>(n)) {
					Node* prod = p->getProducer();
					if (prod->getOpcode() == Opcode::Start && p->getIndex() >= 2)
						return "arg" + std::to_string(p->getIndex() - 2);
					return temp(n);
				}
				return temp(n);
			}

			String binExpr(Node* n) {
				auto* bin = cast<BinaryNode>(n);
				U32 width = n->getType()->isInt() ? n->getType()->getIntWidth() : 0;
				String a = valueExpr(bin->getLHS());
				String b = valueExpr(bin->getRHS());
				String u = "(" + intCType(width, false) + ")";
				String s = "(" + intCType(width, true) + ")";
				switch (n->getOpcode()) {
				case Opcode::Add:
					return a + " + " + b;
				case Opcode::Sub:
					return a + " - " + b;
				case Opcode::Mul:
					return a + " * " + b;
				case Opcode::SDiv:
					return a + " / " + b;
				case Opcode::UDiv:
					return u + a + " / " + u + b;
				case Opcode::SRem:
					return a + " % " + b;
				case Opcode::URem:
					return u + a + " % " + u + b;
				case Opcode::And:
					return a + " & " + b;
				case Opcode::Or:
					return a + " | " + b;
				case Opcode::Xor:
					return a + " ^ " + b;
				case Opcode::Shl:
					return a + " << " + b;
				case Opcode::LShr:
					return s + "(" + u + a + " >> " + b + ")";
				case Opcode::AShr:
					return s + a + " >> " + b;
				default:
					return "0"; // not a binary opcode
				}
			}

			String cmpExpr(Node* n) {
				auto* cmp = cast<CompareNode>(n);
				const Type* ot = cmp->getLHS()->getType();
				U32 width = ot->isInt() ? ot->getIntWidth() : 0;
				String a = valueExpr(cmp->getLHS());
				String b = valueExpr(cmp->getRHS());
				String u = "(" + intCType(width, false) + ")";
				switch (n->getOpcode()) {
				case Opcode::Eq:
					return a + " == " + b;
				case Opcode::Ne:
					return a + " != " + b;
				case Opcode::Slt:
					return a + " < " + b;
				case Opcode::Sle:
					return a + " <= " + b;
				case Opcode::Ult:
					return u + a + " < " + u + b;
				case Opcode::Ule:
					return u + a + " <= " + u + b;
				default:
					return "0"; // not a compare opcode
				}
			}

			String convExpr(Node* n) {
				auto* cv = cast<ConvertNode>(n);
				Node* src = cv->getOperand();
				U32 dstW = n->getType()->getIntWidth();
				U32 srcW = src->getType()->getIntWidth();
				String a = valueExpr(src);
				switch (n->getOpcode()) {
				case Opcode::Trunc:
				case Opcode::SExt:
					return "(" + intCType(dstW, true) + ")" + a;
				case Opcode::ZExt:
					return "(" + intCType(dstW, true) + ")(" + intCType(srcW, false) +
								 ")" + a;
				default:
					return "0"; // not a convert opcode
				}
			}

			String loadExpr(Node* n) {
				auto* l = cast<LoadNode>(n);
				String ptrTy = cType(n->getType(), true) + " *";
				return "*(" + ptrTy + ")(" + valueExpr(l->getPointer()) + ")";
			}

			void emit() {
				printSignature(fn, os);
				os << " {\n";

				for (const Node* nc : fn) {
					Node* n = const_cast<Node*>(nc);
					if (!needTemp.count(n))
						continue;
					os << "  " << cType(n->getType()) << " " << temp(n) << ";\n";
				}
				if (!needTemp.empty())
					os << "\n";

				// only label blocks that are jump targets
				I32 nb = sched.numBlocks();
				List<char> isTarget(nb, 0);
				for (I32 b = 0; b < nb; ++b) {
					const Schedule::Block& blk = sched.block(b);
					if (blk.term == TK::Branch) {
						isTarget[blk.thenB] = 1;
						isTarget[blk.elseB] = 1;
					} else if (blk.term == TK::Goto) {
						isTarget[blk.gotoB] = 1;
					}
				}

				for (I32 b : sched.rpo()) {
					if (isTarget[b])
						os << "L" << b << ":;\n";
					for (Node* n : sched.block(b).nodes)
						emitStatement(n);
					emitTerminator(b);
				}

				os << "}\n";
			}

			void emitStatement(Node* n) {
				switch (n->getOpcode()) {
				case Opcode::Store: {
					auto* s = cast<StoreNode>(n);
					String ptrTy = cType(s->getValue()->getType(), true) + " *";
					os << "  *(" << ptrTy << ")(" << valueExpr(s->getPointer())
						 << ") = " << valueExpr(s->getValue()) << ";\n";
					return;
				}
				case Opcode::Load:
					os << "  " << temp(n) << " = " << loadExpr(n) << ";\n";
					return;
				case Opcode::Call:
					emitCall(cast<CallNode>(n));
					return;
				default:
					break;
				}
				String rhs;
				if (isCompareOpcode(n->getOpcode()))
					rhs = cmpExpr(n);
				else if (isConvertOpcode(n->getOpcode()))
					rhs = convExpr(n);
				else if (isUnaryOpcode(n->getOpcode())) {
					auto* un = cast<UnaryNode>(n);
					rhs = (n->getOpcode() == Opcode::Neg ? "-" : "~") +
								valueExpr(un->getOperand());
				} else
					rhs = binExpr(n);
				os << "  " << temp(n) << " = " << rhs << ";\n";
			}

			void emitCall(CallNode* c) {
				std::ostringstream args;
				for (U32 i = 0, e = c->getArgCount(); i < e; ++i) {
					if (i)
						args << ", ";
					args << valueExpr(c->getArg(i));
				}
				Node* valProj = c->projection(CallNode::valueProjIndex());

				os << "  ";
				if (valProj)
					os << temp(valProj) << " = ";
				os << c->getCallee() << "(" << args.str() << ");\n";
			}

			void emitPhiCopies(I32 targetRegionB, I32 predIdx) {
				struct Move {
					PhiNode* dst;
					Node* srcNode; // null once redirected to a scratch
					String srcExpr;
				};
				List<Move> pending;
				for (PhiNode* phi : sched.block(targetRegionB).phis) {
					Node* v = phi->getValue(predIdx);
					if (v == phi)
						continue; // self-copy: no-op
					pending.push_back({phi, v, String()});
				}
				if (pending.empty())
					return;

				auto readsDest = [&](Node* dst, const Move* self) {
					for (const Move& m : pending)
						if (&m != self && m.srcNode == dst)
							return true;
					return false;
				};
				auto srcOf = [&](const Move& m) {
					return m.srcNode ? valueExpr(m.srcNode) : m.srcExpr;
				};

				List<String> lines;
				List<String> scratchDecls;
				U32 scratchN = 0;

				while (!pending.empty()) {
					B32 progress = false;
					for (U32 i = 0; i < (U32)pending.size();) {
						Move& m = pending[i];
						if (readsDest(m.dst, &m)) {
							++i;
							continue;
						}
						lines.push_back(temp(m.dst) + " = " + srcOf(m) + ";");
						pending.erase(pending.begin() + i);
						progress = true;
					}
					if (pending.empty())
						break;
					if (!progress) {
						// break a cycle
						Move& c = pending.front();
						String nm = "__pc" + std::to_string(scratchN++);
						scratchDecls.push_back(cType(c.dst->getType()) + " " + nm + ";");
						lines.push_back(nm + " = " + temp(c.dst) + ";");
						Node* cycleDst = c.dst;
						for (Move& m : pending)
							if (m.srcNode == cycleDst) {
								m.srcNode = nullptr;
								m.srcExpr = nm;
							}
					}
				}

				if (scratchDecls.empty()) {
					for (const String& l : lines)
						os << "  " << l << "\n";
				} else {
					os << "  {\n";
					for (const String& d : scratchDecls)
						os << "    " << d << "\n";
					for (const String& l : lines)
						os << "    " << l << "\n";
					os << "  }\n";
				}
			}

			void emitTerminator(I32 b) {
				const Schedule::Block& blk = sched.block(b);
				switch (blk.term) {
				case TK::Return: {
					auto* r = cast<ReturnNode>(blk.termNode);
					if (r->hasValue())
						os << "  return " << valueExpr(r->getValue()) << ";\n";
					else
						os << "  return;\n";
					return;
				}
				case TK::Branch: {
					auto* iff = cast<IfNode>(blk.termNode);
					os << "  if (" << valueExpr(iff->getPredicate()) << ") goto L"
						 << blk.thenB << "; else goto L" << blk.elseB << ";\n";
					return;
				}
				case TK::Goto:
					emitPhiCopies(blk.gotoB, blk.gotoPredIdx);
					os << "  goto L" << blk.gotoB << ";\n";
					return;
				}
			}
		};
	} // namespace

	void emitC(const Function& fn, std::ostream& os) { CEmitter(fn, os).run(); }

	void emitC(const Module& module, std::ostream& os) {
		os << "#include <stdint.h>\n";
		// verify ptr size
		if (const TargetInfo* t = module.target()) {
			os << "#include <limits.h>\n";
			os << "_Static_assert(sizeof(void *) * CHAR_BIT == "
				 << t->getPointerSizeInBits()
				 << ",\n               \"Rat module built for target '" << t->getName()
				 << "' (" << t->getPointerSizeInBits() << "-bit pointers)\");\n";
		}
		os << "\n";
		for (const Function* fn : module) {
			printSignature(*fn, os);
			os << ";\n";
		}
		os << "\n";
		B32 first = true;
		for (const Function* fn : module) {
			if (!first)
				os << "\n";
			first = false;
			emitC(*fn, os);
		}
	}

	CEmitterPass::CEmitterPass(std::ostream& os) : os(&os) {}

	const char* CEmitterPass::name() const { return "c emit"; }

	B32 CEmitterPass::run(Module& module) {
		emitC(module, *os);
		return false;
	}
} // namespace rat
