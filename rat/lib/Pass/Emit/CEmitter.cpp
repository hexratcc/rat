#include "Pass/Emit/CEmitter.h"

#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "Target/Target.h"

namespace rat {
	CEmitterPass::FunctionEmitter::FunctionEmitter(const Function& fn, std::ostream& os)
	: fn(fn),
		os(os),
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
			if(n->getType()->isPtr()) {
				std::ostringstream oss;
				oss << "((char *)" << cast<ConstantNode>(n)->getValue() << "LL)";
				return oss.str();
			}
			U32 width = n->getType()->getIntWidth();
			I64 raw = cast<ConstantNode>(n)->getValue();
			I64 v = width > 1 ? signExtend(raw, width) : raw;
			std::ostringstream oss;
			oss << v;
			if(width > 32)
				oss << "LL";
			return oss.str();
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
		U32 ptrBytes = fn.getModule().pointerBytes();
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

	String CEmitterPass::intCType(U32 width, B32 isSigned) {
		if(width <= 1)
			return "int"; // booleans
		const C8* s;
		if(width <= 8)
			s = isSigned ? "int8_t" : "uint8_t";
		else if(width <= 16)
			s = isSigned ? "int16_t" : "uint16_t";
		else if(width <= 32)
			s = isSigned ? "int32_t" : "uint32_t";
		else
			s = isSigned ? "int64_t" : "uint64_t";
		// TODO: bigger than 64b
		return s;
	}

	String CEmitterPass::cType(const Type* t, B32 isSigned) {
		if(t->isInt())
			return intCType(t->getIntWidth(), isSigned);
		if(t->isFloat()) {
			switch(t->getFloatWidth()) {
			case 32:
				return "float";
			case 128:
				return "long double";
			default:
				return "double";
			}
		}
		if(t->isPtr())
			return "char *"; // byte-addressed; typed accesses cast
		return "void";
	}

	B32 CEmitterPass::isCompilerBuiltin(const String& name) {
		return name.rfind("__builtin_", 0) == 0 || name.rfind("__atomic_", 0) == 0 ||
					 name.rfind("__sync_", 0) == 0;
	}

	void CEmitterPass::emitSignature(const Function& fn) { emitSignatureInto(fn, *os); }

	void CEmitterPass::emitSignatureInto(const Function& fn, std::ostream& os) {
		os << (fn.returnsValue() ? cType(fn.getReturnType()) : String("void")) << " " << fn.getName()
			 << "(";
		if(fn.getParamCount() == 0)
			os << "void";
		for(U32 i = 0, e = fn.getParamCount(); i < e; ++i) {
			if(i)
				os << ", ";
			os << cType(fn.getParamType(i)) << " arg" << i;
		}
		if(fn.isVariadic())
			os << ", ...";
		os << ")";
	}

	void CEmitterPass::emitPrologue(const Module& module) {
		*os << "#include <stdint.h>\n";
		// verify ptr size
		if(const TargetInfo* t = module.target()) {
			*os << "#include <limits.h>\n";
			*os << "_Static_assert(sizeof(void *) * CHAR_BIT == " << t->getPointerSizeInBits()
					<< ",\n               \"Rat module built for target '" << t->getName() << "' ("
					<< t->getPointerSizeInBits() << "-bit pointers)\");\n";
		}
		*os << "\n";
	}

	void CEmitterPass::emitForwardDecls(const Module& module) {
		for(const Function* fn : module) {
			emitSignature(*fn);
			*os << ";\n";
		}
	}

	void CEmitterPass::emitExternDecls(const Module& module) {
		Set<String> defined;
		for(const Function* fn : module)
			defined.insert(fn->getName());
		Map<String, const Type*> externs; // name -> return type (null = void)
		List<String> order;
		for(const Function* fn : module) {
			for(const Node* nc : *fn) {
				CallNode* c = dyn_cast<CallNode>(const_cast<Node*>(nc));
				if(!c || c->isIndirect())
					continue;
				const String& name = c->getCallee();
				if(defined.count(name) || isCompilerBuiltin(name))
					continue;
				Node* vp = c->projection(CallNode::valueProjIndex());
				const Type* rt = vp ? vp->getType() : nullptr;
				auto it = externs.find(name);
				if(it == externs.end()) {
					externs.emplace(name, rt);
					order.push_back(name);
				} else if(!it->second && rt) {
					it->second = rt; // a value-returning use overrides a void one
				}
			}
		}
		for(const String& name : order)
			*os << "extern " << (externs[name] ? cType(externs[name]) : String("void")) << " " << name
					<< "();\n";

		Set<String> known;
		for(const Function* fn : module)
			known.insert(fn->getName());
		for(const Global* g : module.globals())
			known.insert(g->getName());
		Set<String> emitted;
		for(const Global* g : module.globals())
			for(const Reloc& r : g->getRelocs())
				if(!known.count(r.symbol) && !isCompilerBuiltin(r.symbol) &&
					 emitted.insert(r.symbol).second)
					*os << "extern char " << r.symbol << "[];\n";
		for(const Function* fn : module)
			for(const Node* nc : *fn)
				if(const GlobalNode* gn = dyn_cast<GlobalNode>(const_cast<Node*>(nc))) {
					const String& sym = gn->getSymbol();
					if(!known.count(sym) && !isCompilerBuiltin(sym) && emitted.insert(sym).second)
						*os << "extern char " << sym << "[];\n";
				}
		*os << "\n";
	}

	void CEmitterPass::emitRelocGlobal(const Global& g, U32 size, U32 ptrBytes) {
		const List<U8>& init = g.getInit();
		List<Reloc> rl = g.getRelocs();
		std::sort(
				rl.begin(), rl.end(), [](const Reloc& a, const Reloc& b) { return a.offset < b.offset; });
		auto byteAt = [&](U32 i) -> U32 { return i < init.size() ? init[i] : 0u; };
		String cst = g.isConstant() ? "const " : "";

		*os << cst << "struct __attribute__((packed)) {\n";
		U32 pos = 0, fi = 0;
		for(U32 i = 0; i < rl.size(); ++i) {
			if(rl[i].offset > pos)
				*os << "\tunsigned char b" << fi++ << "[" << (rl[i].offset - pos) << "];\n";
			*os << "\tvoid *p" << i << ";\n";
			pos = rl[i].offset + ptrBytes;
		}
		if(size > pos)
			*os << "\tunsigned char b" << fi++ << "[" << (size - pos) << "];\n";

		*os << "} " << g.getName() << " = {\n";
		pos = 0;
		B32 first = true;
		auto comma = [&]() {
			if(!first)
				*os << ",\n";
			first = false;
		};
		auto byteRun = [&](U32 from, U32 to) {
			comma();
			*os << "\t{";
			for(U32 b = from; b < to; ++b) {
				if(b != from)
					*os << ", ";
				*os << byteAt(b);
			}
			*os << "}";
		};
		for(U32 i = 0; i < rl.size(); ++i) {
			if(rl[i].offset > pos)
				byteRun(pos, rl[i].offset);
			comma();
			*os << "\t(void *)((char *)&" << rl[i].symbol;
			if(rl[i].addend)
				*os << " + (" << rl[i].addend << ")";
			*os << ")";
			pos = rl[i].offset + ptrBytes;
		}
		if(size > pos)
			byteRun(pos, size);
		*os << "\n};\n";
	}

	void CEmitterPass::emitGlobals(const Module& module) {
		U32 ptrBytes = module.pointerBytes();
		B32 anyGlobal = false;
		for(const Global* g : module.globals()) {
			U32 size = g->getType()->byteSize(ptrBytes);
			if(size == 0)
				size = (U32)g->getInit().size();
			const List<U8>& init = g->getInit();
			if(!g->getRelocs().empty()) {
				emitRelocGlobal(*g, size, ptrBytes);
				anyGlobal = true;
				continue;
			}
			*os << (g->isConstant() ? "const " : "") << "unsigned char " << g->getName() << "[" << size
					<< "] = {";
			U32 last = 0;
			for(U32 i = 0; i < size; ++i)
				if(i < init.size() && init[i] != 0)
					last = i + 1;
			if(last == 0)
				last = 1; // at least one element to keep the brace list valid
			for(U32 i = 0; i < last; ++i) {
				if(i)
					*os << ", ";
				*os << (U32)(i < init.size() ? init[i] : 0);
			}
			*os << "};\n";
			anyGlobal = true;
		}
		if(anyGlobal)
			*os << "\n";
	}

	void CEmitterPass::emitFunction(const Function& fn) { FunctionEmitter(fn, *os).run(); }

	void CEmitterPass::emitModule(const Module& module) {
		emitPrologue(module);
		emitForwardDecls(module);
		emitExternDecls(module);
		emitGlobals(module);

		B32 first = true;
		for(const Function* fn : module) {
			if(!first)
				*os << "\n";
			first = false;
			emitFunction(*fn);
		}
	}

	CEmitterPass::CEmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* CEmitterPass::name() const { return "c emit"; }

	B32 CEmitterPass::run(Module& module) {
		emitModule(module);
		return false;
	}
} // namespace rat
