#include "Pass/Emit/CEmitter.h"

#include "CodeGen/Schedule.h"
#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "Target/Target.h"

namespace rat {
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

	void CEmitterPass::emitPrologue(const TargetInfo& target) {
		*os << "#include <stdint.h>\n";
		// verify ptr size
		*os << "#include <limits.h>\n";
		*os << "_Static_assert(sizeof(void *) * CHAR_BIT == " << target.getPointerSizeInBits()
				<< ",\n               \"rat module built for target '" << target.getName() << "' ("
				<< target.getPointerSizeInBits() << "-bit pointers)\");\n";
		*os << "\n";
	}

	void CEmitterPass::emitForwardDecls(const Module& module) {
		for(const Function* fn : module) {
			emitSignature(*fn);
			*os << ";\n";
		}
	}

	void CEmitterPass::emitExternDecls(const Module& module) {
		Set<String> defined; // function names
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

		Set<String>& known = defined;
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
		if(g.isInternal())
			cst = "static " + cst;

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

	void CEmitterPass::emitGlobals(const Module& module, U32 ptrBytes) {
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
			*os << (g->isInternal() ? "static " : "") << (g->isConstant() ? "const " : "")
					<< "unsigned char " << g->getName() << "[" << size << "] = {";
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

	void CEmitterPass::emitFunction(const Function& fn, U32 ptrBytes) {
		FunctionEmitter(fn, *os, ptrBytes).run();
	}

	void CEmitterPass::emitModule(const Module& module, const TargetInfo& target) {
		U32 ptrBytes = target.getPointerSizeInBytes();
		emitPrologue(target);
		emitForwardDecls(module);
		emitExternDecls(module);
		emitGlobals(module, ptrBytes);

		B32 first = true;
		for(const Function* fn : module) {
			if(!first)
				*os << "\n";
			first = false;
			emitFunction(*fn, ptrBytes);
		}
	}

	CEmitterPass::CEmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* CEmitterPass::name() const { return "c emit"; }

	B32 CEmitterPass::run(Module& module, const TargetInfo& target) {
		emitModule(module, target);
		return false;
	}
} // namespace rat
