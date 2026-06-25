#include "Pass/Emit/GraphEmitter.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

namespace rat {
	String GraphEmitterPass::getNodeId(const Function& fn, const Node* n) {
		return "n" + std::to_string(reinterpret_cast<uintptr_t>(&fn) & 0xffffff) + "_" +
					 std::to_string(n->getId());
	}

	void GraphEmitterPass::getStyle(const Node* n, String& label, String& attrs) {
		std::ostringstream l;
		l << "v" << n->getId() << " " << n->getMnemonic();
		switch (n->getOpcode()) {
		case Opcode::Constant:
			l << " " << cast<ConstantNode>(n)->getValue();
			attrs = "shape=note, style=filled, fillcolor=\"#fff2cc\"";
			break;
		case Opcode::Proj: {
			const auto* p = cast<ProjNode>(n);
			l << " #" << p->getIndex();
			if (!p->getLabel().empty())
				l << " " << p->getLabel();
			attrs = "shape=ellipse, style=filled, fillcolor=\"#eeeeee\"";
			break;
		}
		case Opcode::Call:
			l << " \"" << cast<CallNode>(n)->getCallee() << "\"";
			attrs = "shape=box, style=filled, fillcolor=\"#d9d2e9\"";
			break;
		case Opcode::Start:
		case Opcode::Stop:
			attrs = "shape=box, style=\"filled,bold\", fillcolor=\"#f4cccc\"";
			break;
		case Opcode::Region:
			if (cast<RegionNode>(n)->isLoopHeader())
				attrs = "shape=box, style=\"filled,bold\", fillcolor=\"#9fc5e8\"";
			else
				attrs = "shape=box, style=filled, fillcolor=\"#cfe2f3\"";
			break;
		case Opcode::If:
			attrs = "shape=diamond, style=filled, fillcolor=\"#f4cccc\"";
			break;
		case Opcode::Return:
			attrs = "shape=box, style=filled, fillcolor=\"#f4cccc\"";
			break;
		case Opcode::Phi:
			attrs = "shape=ellipse, style=filled, fillcolor=\"#fce5cd\"";
			break;
		case Opcode::Load:
		case Opcode::Store:
			attrs = "shape=box, style=filled, fillcolor=\"#d9ead3\"";
			break;
		default:
			attrs = "shape=ellipse";
			break;
		}
		l << "\\n: " << n->getType()->str();
		label = l.str();
	}

	const C8* GraphEmitterPass::getEdgeStyle(const Node* producer) {
		const Type* t = producer->getType();
		if (t->isControl())
			return "color=\"#cc0000\", penwidth=2"; // control spine
		if (t->isMemory())
			return "color=\"#3c78d8\", style=dashed"; // memory thread
		if (t->isTuple())
			return "color=\"#999999\""; // tuple feeding a proj
		return "color=\"#000000\"";		// data
	}

	void GraphEmitterPass::emitFunctionBody(const Function& fn) {
		for (const Node* n : fn) {
			String label, attrs;
			getStyle(n, label, attrs);
			*os << "  " << getNodeId(fn, n) << " [label=\"" << label << "\", " << attrs << "];\n";
		}
		*os << "\n";
		for (const Node* n : fn) {
			for (U32 i = 0, e = n->getInputCount(); i < e; ++i) {
				const Node* in = n->getInput(i);
				if (!in)
					continue;
				*os << "  " << getNodeId(fn, in) << " -> " << getNodeId(fn, n) << " [" << getEdgeStyle(in);
				if (n->getOpcode() == Opcode::Phi && i >= 1)
					*os << ", label=\"" << (i - 1) << "\"";
				else if (n->getOpcode() == Opcode::Region)
					*os << ", label=\"" << i << "\"";
				*os << "];\n";
			}
		}
	}

	void GraphEmitterPass::emitModule(const Module& module) {
		*os << "digraph rat {\n";
		*os << "  rankdir=TB;\n  node [fontname=\"monospace\", fontsize=10];\n";
		*os << "  edge [fontname=\"monospace\", fontsize=9];\n\n";
		U32 c = 0;
		for (const Function* fn : module) {
			*os << "  subgraph cluster_" << c++ << " {\n";
			*os << "    label=\"" << fn->getName() << "\"; color=\"#cccccc\";\n";
			emitFunctionBody(*fn);
			*os << "  }\n";
		}
		*os << "}\n";
	}

	GraphEmitterPass::GraphEmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* GraphEmitterPass::name() const { return "graph-emitter"; }

	B32 GraphEmitterPass::run(Module& module) {
		emitModule(module);
		return false;
	}
} // namespace rat
