#include "Pass/PrintPass.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	namespace {
		constexpr const char* Reset = "\033[0m";
		constexpr const char* Green = "\033[32m";
		constexpr const char* TempColors[] = {
				"\033[31m", "\033[33m", "\033[34m", "\033[35m", "\033[36m",
				"\033[91m", "\033[93m", "\033[94m", "\033[95m", "\033[96m",
		};
		constexpr U32 TempColorCount = sizeof(TempColors) / sizeof(TempColors[0]);

		String comment(const String& text) { return Green + text + Reset; }

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
				os << comment(
						"  " +
						std::to_string(static_cast<const ConstantNode*>(node)->getValue()));
				break;
			case Opcode::Proj: {
				const auto* proj = static_cast<const ProjNode*>(node);
				os << comment("  #" + std::to_string(proj->getIndex()));
				if (!proj->getLabel().empty())
					os << comment(" \"" + proj->getLabel() + "\"");
				os << comment(" of ") << ref(proj->getProducer());
				return;
			}
			case Opcode::Call:
				os << comment("  \"" + static_cast<const CallNode*>(node)->getCallee() +
											"\"");
				break;
			case Opcode::Region:
				if (static_cast<const RegionNode*>(node)->isLoopHeader())
					os << comment("  loop");
				break;
			default:
				break;
			}

			printOperands(node, os);
		}

		void printFunction(const Function& fn, std::ostream& os) {
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
	} // namespace

	PrintPass::PrintPass(std::ostream& os) : os(&os) {}

	const char* PrintPass::name() const { return "print"; }

	B32 PrintPass::run(Module& module) {
		B32 first = true;
		for (const Function* fn : module) {
			if (!first)
				*os << "\n";
			first = false;
			printFunction(*fn, *os);
		}
		return false;
	}
} // namespace rat
