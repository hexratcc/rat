#include "Pass/Emit/TextEmitter.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	String TextEmitterPass::comment(const String& text) { return Green + text + Reset; }

	String TextEmitterPass::quoteBytes(const List<U8>& bytes) {
		// raw bytes as a quoted string: printable ASCII verbatim, everything
		// else (and " and \) as a \HH hex escape, so the form round-trips
		static const C8* hex = "0123456789abcdef";
		String out = "\"";
		for (U8 b : bytes) {
			if (b >= 0x20 && b <= 0x7e && b != '"' && b != '\\') {
				out.push_back((C8)b);
			} else {
				out.push_back('\\');
				out.push_back(hex[b >> 4]);
				out.push_back(hex[b & 0xf]);
			}
		}
		out.push_back('"');
		return out;
	}

	String TextEmitterPass::ref(const Node* node) {
		if (!node)
			return "<null>";
		U32 id = node->getId();
		return TempColors[id % TempColorCount] + ("v" + std::to_string(id)) + Reset;
	}

	void TextEmitterPass::emitOperands(const Node* node) {
		for (U32 i = 0; i < node->getInputCount(); ++i)
			*os << (i ? ", " : " ") << ref(node->getInput(i));
	}

	void TextEmitterPass::emitNode(const Node* node) {
		*os << "  " << ref(node) << " = " << node->getMnemonic() << " : " << node->getType()->str();

		switch (node->getOpcode()) {
		case Opcode::Constant:
			*os << comment("  " + std::to_string(cast<ConstantNode>(node)->getValue()));
			break;
		case Opcode::Proj: {
			const auto* proj = cast<ProjNode>(node);
			*os << comment("  #" + std::to_string(proj->getIndex()));
			if (!proj->getLabel().empty())
				*os << comment(" \"" + proj->getLabel() + "\"");
			*os << comment(" of ") << ref(proj->getProducer());
			return;
		}
		case Opcode::Call:
			*os << comment("  \"" + cast<CallNode>(node)->getCallee() + "\"");
			break;
		case Opcode::Global:
			*os << comment("  \"" + cast<GlobalNode>(node)->getSymbol() + "\"");
			break;
		case Opcode::Alloc:
			*os << comment("  " + cast<AllocNode>(node)->getAllocType()->str());
			break;
		case Opcode::Region:
			if (cast<RegionNode>(node)->isLoopHeader())
				*os << comment("  loop");
			break;
		default:
			break;
		}

		emitOperands(node);
	}

	void TextEmitterPass::emitFunction(const Function& fn) {
		*os << "func " << fn.getName() << "(";
		for (U32 i = 0; i < fn.getParamCount(); ++i) {
			if (i)
				*os << ", ";
			*os << fn.getParamType(i)->str();
		}
		*os << ") -> " << (fn.returnsValue() ? fn.getReturnType()->str() : "void") << " {\n";

		for (const Node* node : fn) {
			emitNode(node);
			*os << "\n";
		}

		*os << "}\n";
	}

	void TextEmitterPass::emitModule(const Module& module) {
		B32 any = false;
		for (const Global* g : module.globals()) {
			*os << (g->isConstant() ? "const " : "var ") << g->getName() << " : " << g->getType()->str()
					<< " = " << quoteBytes(g->getInit()) << "\n";
			any = true;
		}

		B32 first = true;
		for (const Function* fn : module) {
			if (!first || any)
				*os << "\n";
			first = false;
			emitFunction(*fn);
		}
	}

	TextEmitterPass::TextEmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* TextEmitterPass::name() const { return "text-emitter"; }

	B32 TextEmitterPass::run(Module& module) {
		emitModule(module);
		return false;
	}
} // namespace rat
