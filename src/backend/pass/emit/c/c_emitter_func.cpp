#include "pass/emit/c/c_emitter.h"

#include "codegen/schedule.h"
#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"

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
		Opcode op = n->getOpcode();
		if(op == Opcode::Alloc || op == Opcode::Constant || op == Opcode::Global)
			return false;
		return Schedule::isFloating(n) || op == Opcode::Load;
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

	void CEmitterPass::FunctionEmitter::writeTemp(std::ostream& os, const Node* n) const {
		os << "t" << n->getId();
	}

	void CEmitterPass::FunctionEmitter::writeFloatLiteral(std::ostream& os, Node* n) {
		I64 raw = cast<ConstantNode>(n)->getValue();
		C8 buf[64];
		if(n->getType()->getFloatWidth() == 32) {
			U32 u = (U32)(U64)raw;
			F32 f;
			std::memcpy(&f, &u, sizeof(f));
			std::snprintf(buf, sizeof(buf), "%af", (F64)f);
		} else {
			U64 u = (U64)raw;
			F64 d;
			std::memcpy(&d, &u, sizeof(d));
			std::snprintf(buf, sizeof(buf), n->getType()->getFloatWidth() == 128 ? "%aL" : "%a", d);
		}
		os << buf;
	}

	void CEmitterPass::FunctionEmitter::writeValue(std::ostream& os, Node* n) {
		if(n->getOpcode() == Opcode::Constant) {
			if(n->getType()->isFloat()) {
				writeFloatLiteral(os, n);
				return;
			}
			if(n->getType()->isPtr()) {
				os << "((char *)" << cast<ConstantNode>(n)->getValue() << "LL)";
				return;
			}
			U32 width = n->getType()->getIntWidth();
			I64 raw = cast<ConstantNode>(n)->getValue();
			I64 v = width > 1 ? signExtend(raw, width) : raw;
			os << v << (width > 32 ? "LL" : "");
			return;
		}
		if(const ProjNode* p = dyn_cast<ProjNode>(n)) {
			Node* prod = p->getProducer();
			if(prod->getOpcode() == Opcode::Start && p->getIndex() >= 2) {
				os << "arg" << (p->getIndex() - 2);
				return;
			}
			writeTemp(os, n);
			return;
		}
		if(GlobalNode* g = dyn_cast<GlobalNode>(n)) {
			os << "((char *)&" << g->getSymbol() << ")";
			return;
		}
		if(isa<AllocNode>(n)) {
			os << "((char *)";
			writeTemp(os, n);
			os << ")";
			return;
		}
		writeTemp(os, n);
	}

	void CEmitterPass::FunctionEmitter::writeBin(std::ostream& os, Node* n) {
		auto* bin = cast<BinaryNode>(n);
		U32 width = n->getType()->isInt() ? n->getType()->getIntWidth() : 0;
		Opcode op = n->getOpcode();
		switch(op) {
		case Opcode::UDiv:
			// unsigned semantics need both sides cast
			os << "(" << intCType(width, false) << ")";
			writeValue(os, bin->getLHS());
			os << " / (" << intCType(width, false) << ")";
			writeValue(os, bin->getRHS());
			return;
		case Opcode::URem:
			os << "(" << intCType(width, false) << ")";
			writeValue(os, bin->getLHS());
			os << " % (" << intCType(width, false) << ")";
			writeValue(os, bin->getRHS());
			return;
		case Opcode::LShr:
			os << "(" << intCType(width, true) << ")((" << intCType(width, false) << ")";
			writeValue(os, bin->getLHS());
			os << " >> ";
			writeValue(os, bin->getRHS());
			os << ")";
			return;
		case Opcode::AShr:
			os << "(" << intCType(width, true) << ")";
			writeValue(os, bin->getLHS());
			os << " >> ";
			writeValue(os, bin->getRHS());
			return;
		case Opcode::Rotl:
		case Opcode::Rotr: {
			U32 wm = width ? width - 1 : 63;
			U32 w = width ? width : 64;
			// "((uN)a << ((b) & wm))" / "((uN)a >> ((w - (b)) & wm))"
			auto half = [&](B32 left) {
				os << "((" << intCType(width, false) << ")";
				writeValue(os, bin->getLHS());
				if(left) {
					os << " << ((";
					writeValue(os, bin->getRHS());
					os << ") & " << wm << "))";
				} else {
					os << " >> ((" << w << " - (";
					writeValue(os, bin->getRHS());
					os << ")) & " << wm << "))";
				}
			};
			os << "(" << intCType(width, true) << ")(";
			half(op == Opcode::Rotl);
			os << " | ";
			half(op != Opcode::Rotl);
			os << ")";
			return;
		}
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
				nullptr, nullptr,        // Rotl Rotr
				"+", "-", "*", "/",      // FAdd FSub FMul FDiv
		};
		// clang-format on
		static_assert(sizeof(kInfix) / sizeof(kInfix[0]) == (U32)Opcode::FDiv - (U32)Opcode::Add + 1,
									"kInfix must cover Add..FDiv");
		U32 idx = (U32)op - (U32)Opcode::Add;
		if(idx >= sizeof(kInfix) / sizeof(kInfix[0]) || !kInfix[idx]) {
			os << "0"; // not a binary opcode
			return;
		}
		writeValue(os, bin->getLHS());
		os << " " << kInfix[idx] << " ";
		writeValue(os, bin->getRHS());
	}

	void CEmitterPass::FunctionEmitter::writeCmp(std::ostream& os, Node* n) {
		auto* cmp = cast<CompareNode>(n);
		const Type* ot = cmp->getLHS()->getType();
		U32 width = ot->isInt() ? ot->getIntWidth() : 0;
		Opcode op = n->getOpcode();
		if(!isCompareOpcode(op)) {
			os << "0";
			return;
		}
		static const C8* const kInfix[] = {
				"==", "!=", "<", "<=", "<", "<=", "==", "!=", "<", "<=", ">", ">="};
		U32 idx = (U32)op - (U32)Opcode::Eq;
		if(op == Opcode::Ult || op == Opcode::Ule) {
			const C8* u = intCType(width, false);
			os << "(" << u << ")";
			writeValue(os, cmp->getLHS());
			os << " " << kInfix[idx] << " (" << u << ")";
			writeValue(os, cmp->getRHS());
			return;
		}
		writeValue(os, cmp->getLHS());
		os << " " << kInfix[idx] << " ";
		writeValue(os, cmp->getRHS());
	}

	void CEmitterPass::FunctionEmitter::writeConv(std::ostream& os, Node* n) {
		auto* cv = cast<ConvertNode>(n);
		Node* src = cv->getOperand();
		if(n->getType()->isPtr()) {
			os << "((char *)(";
			writeValue(os, src);
			os << "))";
			return;
		}
		if(src->getType()->isPtr()) {
			os << "((" << intCType(n->getType()->getIntWidth(), true) << ")(";
			writeValue(os, src);
			os << "))";
			return;
		}
		U32 dstW = n->getType()->getIntWidth();
		U32 srcW = src->getType()->getIntWidth();
		switch(n->getOpcode()) {
		case Opcode::Trunc:
		case Opcode::SExt:
			os << "(" << intCType(dstW, true) << ")";
			break;
		case Opcode::ZExt:
			os << "(" << intCType(dstW, true) << ")(" << intCType(srcW, false) << ")";
			break;
		case Opcode::SIToFP:
		case Opcode::FPExt:
		case Opcode::FPTrunc:
			os << "(" << cType(n->getType()) << ")";
			break;
		case Opcode::UIToFP:
			os << "(" << cType(n->getType()) << ")(" << intCType(srcW, false) << ")";
			break;
		case Opcode::FPToSI:
			os << "(" << intCType(dstW, true) << ")";
			break;
		case Opcode::FPToUI:
			os << "(" << intCType(dstW, false) << ")";
			break;
		default:
			os << "0"; // not a convert opcode
			return;
		}
		writeValue(os, src);
	}

	void CEmitterPass::FunctionEmitter::writeLoad(std::ostream& os, Node* n) {
		auto* l = cast<LoadNode>(n);
		os << "*(" << cType(n->getType(), true) << " *)(";
		writeValue(os, l->getPointer());
		os << ")";
	}

	void CEmitterPass::FunctionEmitter::emit() {
		CEmitterPass::emitSignatureInto(fn, os);
		os << " {\n";

		for(const Node* nc : fn) {
			Node* n = const_cast<Node*>(nc);
			if(!needTemp.count(n))
				continue;
			os << "  " << cType(n->getType()) << " ";
			writeTemp(os, n);
			os << ";\n";
		}

		// stack allocations become local byte buffers
		B32 anyAlloc = false;
		for(const Node* nc : fn) {
			if(AllocNode* a = dyn_cast<AllocNode>(const_cast<Node*>(nc))) {
				if(a->isVariableSized()) {
					os << "  unsigned char *";
					writeTemp(os, a);
					os << ";\n";
				} else {
					U32 size = a->getAllocType()->byteSize(ptrBytes);
					if(size == 0)
						size = 1;
					os << "  unsigned char ";
					writeTemp(os, a);
					os << "[" << size << "];\n";
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
			} else if(blk.term == TK::Switch) {
				for(I32 t : blk.caseB)
					isTarget[t] = 1;
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
		case Opcode::Constant:
		case Opcode::Global:
			return;
		case Opcode::Store: {
			auto* s = cast<StoreNode>(n);
			os << "  *(" << cType(s->getValue()->getType(), true) << " *)(";
			writeValue(os, s->getPointer());
			os << ") = ";
			writeValue(os, s->getValue());
			os << ";\n";
			return;
		}
		case Opcode::Load:
			os << "  ";
			writeTemp(os, n);
			os << " = ";
			writeLoad(os, n);
			os << ";\n";
			return;
		case Opcode::Call:
			emitCall(cast<CallNode>(n));
			return;
		case Opcode::Alloc: {
			auto* a = cast<AllocNode>(n);
			if(a->isVariableSized()) {
				os << "  ";
				writeTemp(os, a);
				os << " = __builtin_alloca(";
				writeValue(os, a->getSizeOperand());
				os << ");\n";
			}
			return;
		}
		default:
			break;
		}
		os << "  ";
		writeTemp(os, n);
		os << " = ";
		if(isCompareOpcode(n->getOpcode()))
			writeCmp(os, n);
		else if(isConvertOpcode(n->getOpcode()))
			writeConv(os, n);
		else if(isUnaryOpcode(n->getOpcode())) {
			auto* un = cast<UnaryNode>(n);
			os << (n->getOpcode() == Opcode::Not ? "~" : "-");
			writeValue(os, un->getOperand());
		} else
			writeBin(os, n);
		os << ";\n";
	}

	B32 CEmitterPass::FunctionEmitter::isVoidBuiltin(const String& name) {
		return name == "__sync_synchronize" || name == "__sync_lock_release" ||
					 name == "__atomic_store" || name == "__atomic_store_n" || name == "__atomic_load" ||
					 name == "__atomic_thread_fence" || name == "__atomic_signal_fence";
	}

	void CEmitterPass::FunctionEmitter::writeArgs(std::ostream& os, CallNode* c) {
		for(U32 i = 0, e = c->getArgCount(); i < e; ++i) {
			if(i)
				os << ", ";
			writeValue(os, c->getArg(i));
		}
	}

	void CEmitterPass::FunctionEmitter::emitCall(CallNode* c) {
		Node* valProj = c->projection(CallNode::valueProjIndex());

		if(valProj && !c->isIndirect() && isVoidBuiltin(c->getCallee())) {
			os << "  ";
			writeTemp(os, valProj);
			os << " = (" << c->getCallee() << "(";
			writeArgs(os, c);
			os << "), 0);\n";
			return;
		}

		os << "  ";
		if(valProj) {
			writeTemp(os, valProj);
			os << " = ";
		}
		if(c->isIndirect()) {
			os << "((" << (valProj ? cType(valProj->getType()) : "void") << " (*)(";
			for(U32 i = 0, e = c->getArgCount(); i < e; ++i) {
				if(i)
					os << ", ";
				os << cType(c->getArg(i)->getType());
			}
			if(c->getArgCount() == 0)
				os << "void";
			os << "))";
			writeValue(os, c->getTarget());
			os << ")(";
			writeArgs(os, c);
			os << ");\n";
		} else if(emitVaIntrinsic(c, valProj)) {
			return;
		} else {
			os << c->getCallee() << "(";
			writeArgs(os, c);
			os << ");\n";
		}
	}

	B32 CEmitterPass::FunctionEmitter::emitVaIntrinsic(CallNode* c, Node* valProj) {
		const String& callee = c->getCallee();
		auto vaList = [&](U32 argIdx) {
			os << "*(__builtin_va_list *)(";
			writeValue(os, c->getArg(argIdx));
			os << ")";
		};
		if(callee == "__builtin_va_arg") {
			os << "__builtin_va_arg(";
			vaList(0);
			os << ", " << cType(valProj->getType()) << ");\n";
			return true;
		}
		if(callee == "__builtin_va_start") {
			os << "__builtin_va_start(";
			vaList(0);
			os << ", ";
			writeValue(os, c->getArg(1));
			os << ");\n";
			return true;
		}
		if(callee == "__builtin_va_end") {
			os << "__builtin_va_end(";
			vaList(0);
			os << ");\n";
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
		auto moveLine = [&](const Move& m) {
			std::ostringstream ls;
			writeTemp(ls, m.dst);
			ls << " = (" << cType(m.dst->getType()) << ")(";
			if(m.srcNode)
				writeValue(ls, m.srcNode);
			else
				ls << m.srcExpr;
			ls << ");";
			return ls.str();
		};

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
				lines.push_back(moveLine(m));
				pending.erase(pending.begin() + i);
				progress = true;
			}
			if(pending.empty())
				break;
			if(!progress) {
				// break a cycle
				Move& c = pending.front();
				String nm = "__pc" + std::to_string(scratchN++);
				std::ostringstream ds;
				ds << cType(c.dst->getType()) << " " << nm << ";";
				scratchDecls.push_back(ds.str());
				std::ostringstream ms;
				ms << nm << " = ";
				writeTemp(ms, c.dst);
				ms << ";";
				lines.push_back(ms.str());
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
			if(r->hasValue()) {
				os << "  return ";
				writeValue(os, r->getValue());
				os << ";\n";
			} else {
				os << "  return;\n";
			}
			return;
		}
		case TK::Branch: {
			auto* iff = cast<IfNode>(blk.termNode);
			os << "  if (";
			writeValue(os, iff->getPredicate());
			os << ") goto L" << blk.thenB << "; else goto L" << blk.elseB << ";\n";
			return;
		}
		case TK::Switch: {
			auto* sw = cast<SwitchNode>(blk.termNode);
			os << "  switch (";
			writeValue(os, sw->getSelector());
			os << ") {\n";
			for(U32 i = 0; i + 1 < (U32)blk.caseB.size(); ++i)
				os << "  case " << i << ": goto L" << blk.caseB[i] << ";\n";
			// selector is range-checked, so the last slot doubles as default
			os << "  default: goto L" << blk.caseB.back() << ";\n  }\n";
			return;
		}
		case TK::Goto:
			emitPhiCopies(blk.gotoB, blk.gotoPredIdx);
			os << "  goto L" << blk.gotoB << ";\n";
			return;
		}
	}
} // namespace rat
