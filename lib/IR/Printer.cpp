#include "IR/Printer.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	namespace {
		String ref(const Node* node) {
			if (!node)
				return "<null>";
			return "v" + std::to_string(node->getId());
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
				os << "  " << static_cast<const ConstantNode*>(node)->getValue();
				break;
			case Opcode::Proj: {
				const auto* proj = static_cast<const ProjNode*>(node);
				os << "  #" << proj->getIndex();
				if (!proj->getLabel().empty())
					os << " \"" << proj->getLabel() << "\"";
				os << " of " << ref(proj->getProducer());
				return;
			}
			case Opcode::Call:
				os << "  \"" << static_cast<const CallNode*>(node)->getCallee() << "\"";
				break;
			case Opcode::Region:
				if (static_cast<const RegionNode*>(node)->isLoopHeader())
					os << "  loop";
				break;
			default:
				break;
			}

			printOperands(node, os);
		}
	} // namespace

	void print(const Function& fn, std::ostream& os) {
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

	void print(const Module& module, std::ostream& os) {
		B32 first = true;
		for (const Function* fn : module) {
			if (!first)
				os << "\n";
			first = false;
			print(*fn, os);
		}
	}
} // namespace rat
