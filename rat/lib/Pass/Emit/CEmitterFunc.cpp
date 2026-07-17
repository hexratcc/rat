#include "Pass/Emit/CEmitter.h"

#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	CEmitterPass::FunctionEmitter::FunctionEmitter(const Function& fn, std::ostream& os, U32 ptrBytes)
	: fn(fn),
		os(os),
		ptrBytes(ptrBytes),
		sched(fn) {}

	void CEmitterPass::FunctionEmitter::run() {
		computeNeedTemp();
		emit();
	}

	B32 CEmitterPass::FunctionEmitter::producesTemp(const Node* n) {
		if(n->getOpcode() == Opcode::Alloc)
			return false;
		return Schedule::isFloating(n) || n->getOpcode() == Opcode::Load;
	}

	void CEmitterPass::FunctionEmitter::computeNeedTemp() {
		for(I32 b = 0; b < sched.numBlocks(); ++b) {
			for(PhiNode* phi : sched.block(b).phis)
				needTemp.insert(phi); // data phis only
			for(Node* n : sched.block(b).nodes) {
				if(producesTemp(n))
					needTemp.insert(n);
				if(n->getOpcode() == Opcode::Call)
					if(ProjNode* p = n->projection(CallNode::valueProjIndex()))
						needTemp.insert(p);
			}
		}
	}

	String CEmitterPass::FunctionEmitter::temp(const Node* n) const {
		return "t" + std::to_string(n->getId());
	}

	String CEmitterPass::FunctionEmitter::floatLiteral(Node* n) {
		I64 raw = cast<ConstantNode>(n)->getValue();
		C8 buf[64];
		if(n->getType()->getFloatWidth() == 32) {
			U32 u = (U32)(U64)raw;
			F32 f;
			std::memcpy(&f, &u, sizeof(f));
			std::snprintf(buf, sizeof(buf), "%af", (F64)f);
		} else if(n->getType()->getFloatWidth() == 128) {
			U64 u = (U64)raw;
			F64 d;
			std::memcpy(&d, &u, sizeof(d));
			std::snprintf(buf, sizeof(buf), "%aL", d);
		} else {
			U64 u = (U64)raw;
			F64 d;
			std::memcpy(&d, &u, sizeof(d));
			std::snprintf(buf, sizeof(buf), "%a", d);
		}
		return String(buf);
	}

	String CEmitterPass::FunctionEmitter::valueExpr(Node* n) {
		if(n->getOpcode() == Opcode::Constant) {
			if(n->getType()->isFloat())
				return floatLiteral(n);
			if(n->getType()->isPtr())
				return "((char *)" + std::to_string(cast<ConstantNode>(n)->getValue()) + "LL)";
			U32 width = n->getType()->getIntWidth();
			I64 raw = cast<ConstantNode>(n)->getValue();
			I64 v = width > 1 ? signExtend(raw, width) : raw;
			return std::to_string(v) + (width > 32 ? "LL" : "");
		}
		if(const ProjNode* p = dyn_cast<ProjNode>(n)) {
			Node* prod = p->getProducer();
			if(prod->getOpcode() == Opcode::Start && p->getIndex() >= 2)
				return "arg" + std::to_string(p->getIndex() - 2);
			return temp(n);
		}
		if(GlobalNode* g = dyn_cast<GlobalNode>(n))
			return "((char *)&" + g->getSymbol() + ")";
		if(isa<AllocNode>(n))
			return "((char *)" + temp(n) + ")";
		return temp(n);
	}

	String CEmitterPass::FunctionEmitter::binExpr(Node* n) {
		auto* bin = cast<BinaryNode>(n);
		U32 width = n->getType()->isInt() ? n->getType()->getIntWidth() : 0;
		String a = valueExpr(bin->getLHS());
		String b = valueExpr(bin->getRHS());
		String u = "(" + intCType(width, false) + ")";
		String s = "(" + intCType(width, true) + ")";
		Opcode op = n->getOpcode();
		switch(op) {
		case Opcode::UDiv:
			return u + a + " / " + u + b; // unsigned semantics need both sides cast
		case Opcode::URem:
			return u + a + " % " + u + b;
		case Opcode::LShr:
			return s + "(" + u + a + " >> " + b + ")";
		case Opcode::AShr:
			return s + a + " >> " + b;
		default:
			break;
		}
		// clang-format off
		static const C8* const kInfix[] = {
				"+", "-", "*",           // Add Sub Mul
				"/", nullptr,            // SDiv UDiv
				"%", nullptr,            // SRem URem
				"&", "|", "^",           // And Or Xor
				"<<", nullptr, nullptr,  // Shl LShr AShr
				"+", "-", "*", "/",      // FAdd FSub FMul FDiv
		};
		// clang-format on
		static_assert(sizeof(kInfix) / sizeof(kInfix[0]) == (U32)Opcode::FDiv - (U32)Opcode::Add + 1,
									"kInfix must cover Add..FDiv");
		U32 idx = (U32)op - (U32)Opcode::Add;
		if(idx >= sizeof(kInfix) / sizeof(kInfix[0]) || !kInfix[idx])
			return "0"; // not a binary opcode
		return a + " " + kInfix[idx] + " " + b;
	}

	String CEmitterPass::FunctionEmitter::cmpExpr(Node* n) {
		auto* cmp = cast<CompareNode>(n);
		const Type* ot = cmp->getLHS()->getType();
		U32 width = ot->isInt() ? ot->getIntWidth() : 0;
		String a = valueExpr(cmp->getLHS());
		String b = valueExpr(cmp->getRHS());
		Opcode op = n->getOpcode();
		if(!isCompareOpcode(op))
			return "0";
		static const C8* const kInfix[] = {
				"==", "!=", "<", "<=", "<", "<=", "==", "!=", "<", "<=", ">", ">="};
		U32 idx = (U32)op - (U32)Opcode::Eq;
		if(op == Opcode::Ult || op == Opcode::Ule) {
			String u = "(" + intCType(width, false) + ")";
			return u + a + " " + kInfix[idx] + " " + u + b;
		}
		return a + " " + kInfix[idx] + " " + b;
	}

	String CEmitterPass::FunctionEmitter::convExpr(Node* n) {
		auto* cv = cast<ConvertNode>(n);
		Node* src = cv->getOperand();
		String a = valueExpr(src);
		if(n->getType()->isPtr())
			return "((char *)(" + a + "))";
		if(src->getType()->isPtr())
			return "((" + intCType(n->getType()->getIntWidth(), true) + ")(" + a + "))";
		U32 dstW = n->getType()->getIntWidth();
		U32 srcW = src->getType()->getIntWidth();
		switch(n->getOpcode()) {
		case Opcode::Trunc:
		case Opcode::SExt:
			return "(" + intCType(dstW, true) + ")" + a;
		case Opcode::ZExt:
			return "(" + intCType(dstW, true) + ")(" + intCType(srcW, false) + ")" + a;
		case Opcode::SIToFP:
		case Opcode::FPExt:
		case Opcode::FPTrunc:
			return "(" + cType(n->getType()) + ")" + a;
		case Opcode::UIToFP:
			return "(" + cType(n->getType()) + ")(" + intCType(srcW, false) + ")" + a;
		case Opcode::FPToSI:
			return "(" + intCType(dstW, true) + ")" + a;
		case Opcode::FPToUI:
			return "(" + intCType(dstW, false) + ")" + a;
		default:
			return "0"; // not a convert opcode
		}
	}

	String CEmitterPass::FunctionEmitter::loadExpr(Node* n) {
		auto* l = cast<LoadNode>(n);
		String ptrTy = cType(n->getType(), true) + " *";
		return "*(" + ptrTy + ")(" + valueExpr(l->getPointer()) + ")";
	}

	void CEmitterPass::FunctionEmitter::emit() {
		CEmitterPass::emitSignatureInto(fn, os);
		os << " {\n";

		for(const Node* nc : fn) {
			Node* n = const_cast<Node*>(nc);
			if(!needTemp.count(n))
				continue;
			os << "  " << cType(n->getType()) << " " << temp(n) << ";\n";
		}

		// stack allocations become local byte buffers
		B32 anyAlloc = false;
		for(const Node* nc : fn) {
			if(AllocNode* a = dyn_cast<AllocNode>(const_cast<Node*>(nc))) {
				if(a->isVariableSized()) {
					os << "  unsigned char *" << temp(a) << ";\n";
				} else {
					U32 size = a->getAllocType()->byteSize(ptrBytes);
					if(size == 0)
						size = 1;
					os << "  unsigned char " << temp(a) << "[" << size << "];\n";
				}
				anyAlloc = true;
			}
		}

		if(!needTemp.empty() || anyAlloc)
			os << "\n";

		// only label blocks that are jump targets
		I32 nb = sched.numBlocks();
		List<C8> isTarget(nb, 0);
		for(I32 b = 0; b < nb; ++b) {
			const Schedule::Block& blk = sched.block(b);
			if(blk.term == TK::Branch) {
				isTarget[blk.thenB] = 1;
				isTarget[blk.elseB] = 1;
			} else if(blk.term == TK::Goto) {
				isTarget[blk.gotoB] = 1;
			}
		}

		for(I32 b : sched.rpo()) {
			if(isTarget[b])
				os << "L" << b << ":;\n";
			for(Node* n : sched.block(b).nodes)
				emitStatement(n);
			emitTerminator(b);
		}

		os << "}\n";
	}

	void CEmitterPass::FunctionEmitter::emitStatement(Node* n) {
		switch(n->getOpcode()) {
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
		case Opcode::Alloc: {
			auto* a = cast<AllocNode>(n);
			if(a->isVariableSized())
				os << "  " << temp(a) << " = __builtin_alloca(" << valueExpr(a->getSizeOperand()) << ");\n";
			return;
		}
		default:
			break;
		}
		String rhs;
		if(isCompareOpcode(n->getOpcode()))
			rhs = cmpExpr(n);
		else if(isConvertOpcode(n->getOpcode()))
			rhs = convExpr(n);
		else if(isUnaryOpcode(n->getOpcode())) {
			auto* un = cast<UnaryNode>(n);
			rhs = (n->getOpcode() == Opcode::Not ? "~" : "-") + valueExpr(un->getOperand());
		} else
			rhs = binExpr(n);
		os << "  " << temp(n) << " = " << rhs << ";\n";
	}

	B32 CEmitterPass::FunctionEmitter::isVoidBuiltin(const String& name) {
		return name == "__sync_synchronize" || name == "__sync_lock_release" ||
					 name == "__atomic_store" || name == "__atomic_store_n" || name == "__atomic_load" ||
					 name == "__atomic_thread_fence" || name == "__atomic_signal_fence";
	}

	void CEmitterPass::FunctionEmitter::emitCall(CallNode* c) {
		std::ostringstream args;
		for(U32 i = 0, e = c->getArgCount(); i < e; ++i) {
			if(i)
				args << ", ";
			args << valueExpr(c->getArg(i));
		}
		Node* valProj = c->projection(CallNode::valueProjIndex());

		if(valProj && !c->isIndirect() && isVoidBuiltin(c->getCallee())) {
			os << "  " << temp(valProj) << " = (" << c->getCallee() << "(" << args.str() << "), 0);\n";
			return;
		}

		os << "  ";
		if(valProj)
			os << temp(valProj) << " = ";
		if(c->isIndirect()) {
			String ret = valProj ? cType(valProj->getType()) : String("void");
			std::ostringstream sig;
			sig << ret << " (*)(";
			for(U32 i = 0, e = c->getArgCount(); i < e; ++i) {
				if(i)
					sig << ", ";
				sig << cType(c->getArg(i)->getType());
			}
			if(c->getArgCount() == 0)
				sig << "void";
			sig << ")";
			os << "((" << sig.str() << ")" << valueExpr(c->getTarget()) << ")(" << args.str() << ");\n";
		} else if(emitVaIntrinsic(c, valProj)) {
			return;
		} else {
			os << c->getCallee() << "(" << args.str() << ");\n";
		}
	}

	B32 CEmitterPass::FunctionEmitter::emitVaIntrinsic(CallNode* c, Node* valProj) {
		const String& callee = c->getCallee();
		auto vaList = [&](U32 argIdx) {
			return "*(__builtin_va_list *)(" + valueExpr(c->getArg(argIdx)) + ")";
		};
		if(callee == "__builtin_va_arg") {
			os << "__builtin_va_arg(" << vaList(0) << ", " << cType(valProj->getType()) << ");\n";
			return true;
		}
		if(callee == "__builtin_va_start") {
			os << "__builtin_va_start(" << vaList(0) << ", " << valueExpr(c->getArg(1)) << ");\n";
			return true;
		}
		if(callee == "__builtin_va_end") {
			os << "__builtin_va_end(" << vaList(0) << ");\n";
			return true;
		}
		return false;
	}

	List<CEmitterPass::FunctionEmitter::Move>
	CEmitterPass::FunctionEmitter::collectPhiMoves(I32 targetRegionB, I32 predIdx) {
		List<Move> moves;
		for(PhiNode* phi : sched.block(targetRegionB).phis) {
			Node* v = phi->getValue(predIdx);
			if(v != phi)
				moves.push_back({phi, v, String()});
		}
		return moves;
	}

	void CEmitterPass::FunctionEmitter::writeMoveBlock(const List<String>& scratchDecls,
																										 const List<String>& lines) {
		if(scratchDecls.empty()) {
			for(const String& l : lines)
				os << "  " << l << "\n";
			return;
		}
		os << "  {\n";
		for(const String& d : scratchDecls)
			os << "    " << d << "\n";
		for(const String& l : lines)
			os << "    " << l << "\n";
		os << "  }\n";
	}

	void CEmitterPass::FunctionEmitter::emitPhiCopies(I32 targetRegionB, I32 predIdx) {
		List<Move> pending = collectPhiMoves(targetRegionB, predIdx);
		if(pending.empty())
			return;

		auto readsDest = [&](Node* dst, const Move* self) {
			for(const Move& m : pending)
				if(&m != self && m.srcNode == dst)
					return true;
			return false;
		};
		auto srcOf = [&](const Move& m) { return m.srcNode ? valueExpr(m.srcNode) : m.srcExpr; };

		List<String> lines;
		List<String> scratchDecls;
		U32 scratchN = 0;

		while(!pending.empty()) {
			B32 progress = false;
			for(U32 i = 0; i < (U32)pending.size();) {
				Move& m = pending[i];
				if(readsDest(m.dst, &m)) {
					++i;
					continue;
				}
				lines.push_back(temp(m.dst) + " = (" + cType(m.dst->getType()) + ")(" + srcOf(m) + ");");
				pending.erase(pending.begin() + i);
				progress = true;
			}
			if(pending.empty())
				break;
			if(!progress) {
				// break a cycle
				Move& c = pending.front();
				String nm = "__pc" + std::to_string(scratchN++);
				scratchDecls.push_back(cType(c.dst->getType()) + " " + nm + ";");
				lines.push_back(nm + " = " + temp(c.dst) + ";");
				Node* cycleDst = c.dst;
				for(Move& m : pending)
					if(m.srcNode == cycleDst) {
						m.srcNode = nullptr;
						m.srcExpr = nm;
					}
			}
		}

		writeMoveBlock(scratchDecls, lines);
	}

	void CEmitterPass::FunctionEmitter::emitTerminator(I32 b) {
		const Schedule::Block& blk = sched.block(b);
		switch(blk.term) {
		case TK::Return: {
			auto* r = cast<ReturnNode>(blk.termNode);
			if(r->hasValue())
				os << "  return " << valueExpr(r->getValue()) << ";\n";
			else
				os << "  return;\n";
			return;
		}
		case TK::Branch: {
			auto* iff = cast<IfNode>(blk.termNode);
			os << "  if (" << valueExpr(iff->getPredicate()) << ") goto L" << blk.thenB << "; else goto L"
				 << blk.elseB << ";\n";
			return;
		}
		case TK::Goto:
			emitPhiCopies(blk.gotoB, blk.gotoPredIdx);
			os << "  goto L" << blk.gotoB << ";\n";
			return;
		}
	}
} // namespace rat
