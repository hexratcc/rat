#include "Pass/Emit/TextEmitter.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	namespace detail {
		constexpr const char* Reset = "\033[0m";
		constexpr const char* Green = "\033[32m";
		constexpr const char* TempColors[] = {
				"\033[31m", "\033[33m", "\033[34m", "\033[35m", "\033[36m",
				"\033[91m", "\033[93m", "\033[94m", "\033[95m", "\033[96m",
		};
		constexpr U32 TempColorCount = sizeof(TempColors) / sizeof(TempColors[0]);

		String comment(const String& text) { return Green + text + Reset; }

		// raw bytes as a quoted string: printable ASCII verbatim, everything
		// else (and " and \) as a \HH hex escape, so the form round-trips
		String quoteBytes(const List<U8>& bytes) {
			static const char* hex = "0123456789abcdef";
			String out = "\"";
			for (U8 b : bytes) {
				if (b >= 0x20 && b <= 0x7e && b != '"' && b != '\\') {
					out.push_back((char)b);
				} else {
					out.push_back('\\');
					out.push_back(hex[b >> 4]);
					out.push_back(hex[b & 0xf]);
				}
			}
			out.push_back('"');
			return out;
		}

		String ref(const Node* node) {
			if (!node)
				return "<null>";
			U32 id = node->getId();
			return TempColors[id % TempColorCount] + ("v" + std::to_string(id)) +
						 Reset;
		}

		void printOperands(const Node* node, std::ostream& os) {
			for (U32 i = 0; i < node->getInputCount(); ++i)
				os << (i ? ", " : " ") << ref(node->getInput(i));
		}

		void printNode(const Node* node, std::ostream& os) {
			os << "  " << ref(node) << " = " << node->getMnemonic() << " : "
				 << node->getType()->str();

			switch (node->getOpcode()) {
			case Opcode::Constant:
				os << comment("  " +
											std::to_string(cast<ConstantNode>(node)->getValue()));
				break;
			case Opcode::Proj: {
				const auto* proj = cast<ProjNode>(node);
				os << comment("  #" + std::to_string(proj->getIndex()));
				if (!proj->getLabel().empty())
					os << comment(" \"" + proj->getLabel() + "\"");
				os << comment(" of ") << ref(proj->getProducer());
				return;
			}
			case Opcode::Call:
				os << comment("  \"" + cast<CallNode>(node)->getCallee() + "\"");
				break;
			case Opcode::Global:
				os << comment("  \"" + cast<GlobalNode>(node)->getSymbol() + "\"");
				break;
			case Opcode::Alloc:
				os << comment("  " + cast<AllocNode>(node)->getAllocType()->str());
				break;
			case Opcode::Region:
				if (cast<RegionNode>(node)->isLoopHeader())
					os << comment("  loop");
				break;
			default:
				break;
			}

			printOperands(node, os);
		}
	} // namespace detail
	using namespace detail;

	void emitText(const Function& fn, std::ostream& os) {
		os << "func " << fn.getName() << "(";
		for (U32 i = 0; i < fn.getParamCount(); ++i) {
			if (i)
				os << ", ";
			os << fn.getParamType(i)->str();
		}
		os << ") -> " << (fn.returnsValue() ? fn.getReturnType()->str() : "void")
			 << " {\n";

		for (const Node* node : fn) {
			printNode(node, os);
			os << "\n";
		}

		os << "}\n";
	}

	void emitText(const Module& module, std::ostream& os) {
		B32 any = false;
		for (const Global* g : module.globals()) {
			os << (g->isConstant() ? "const " : "var ") << g->getName() << " : "
				 << g->getType()->str() << " = " << quoteBytes(g->getInit()) << "\n";
			any = true;
		}

		B32 first = true;
		for (const Function* fn : module) {
			if (!first || any)
				os << "\n";
			first = false;
			emitText(*fn, os);
		}
	}

	TextEmitterPass::TextEmitterPass(std::ostream& os) : os(&os) {}

	const char* TextEmitterPass::name() const { return "text-emitter"; }

	B32 TextEmitterPass::run(Module& module) {
		emitText(module, *os);
		return false;
	}
} // namespace rat
