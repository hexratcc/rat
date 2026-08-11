#include "pass/emit/graph_emitter.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"

namespace rat {
	namespace {
		void writeId(std::ostream& os, U32 fnIndex, const Node* n) {
			os << "f" << fnIndex << "_v" << n->getId();
		}

		// dot label strings: escape quote and backslash
		void writeEscaped(std::ostream& os, const String& s) {
			for(C8 ch : s) {
				if(ch == '"' || ch == '\\')
					os << '\\';
				os << ch;
			}
		}

		void writeHtml(std::ostream& os, const String& s) {
			for(C8 ch : s) {
				switch(ch) {
				case '&': os << "&amp;"; break;
				case '<': os << "&lt;"; break;
				case '>': os << "&gt;"; break;
				default: os << ch; break;
				}
			}
		}

		U32 lcm(U32 a, U32 b) {
			U32 x = a, y = b;
			while(y) {
				U32 t = x % y;
				x = y;
				y = t;
			}
			return a / x * b;
		}

		String portName(const Node* n, U32 i) {
			switch(n->getOpcode()) {
			case Opcode::If:
				return i == 0 ? "ctrl" : "cond";
			case Opcode::Switch:
				return i == 0 ? "ctrl" : "sel";
			case Opcode::Load:
				return i == 0 ? "ctrl" : i == 1 ? "mem" : "addr";
			case Opcode::Store:
				return i == 0 ? "ctrl" : i == 1 ? "mem" : i == 2 ? "addr" : "val";
			case Opcode::Return:
				return i == 0 ? "ctrl" : i == 1 ? "mem" : std::to_string(i - 2);
			case Opcode::Call: {
				if(i == 0)
					return "ctrl";
				if(i == 1)
					return "mem";
				const auto* c = cast<CallNode>(n);
				if(c->isIndirect())
					return i == 2 ? "tgt" : std::to_string(i - 3);
				return std::to_string(i - 2);
			}
			case Opcode::Phi:
				return i == 0 ? "region" : std::to_string(i - 1);
			case Opcode::Proj:
				return "src";
			case Opcode::Stop:
				return "ret";
			case Opcode::Alloc:
				return "size";
			case Opcode::Splat:
				return "val";
			case Opcode::Extract:
			case Opcode::Shuffle:
				return "vec";
			default:
				return std::to_string(i);
			}
		}

		void writeTitle(std::ostream& os, const Node* n) {
			os << "v" << n->getId() << " " << n->getMnemonic();
			switch(n->getOpcode()) {
			case Opcode::Constant:
				os << " " << cast<ConstantNode>(n)->getValue();
				break;
			case Opcode::Proj: {
				const auto* p = cast<ProjNode>(n);
				os << " #" << p->getIndex();
				if(*p->getLabel()) {
					os << " ";
					writeHtml(os, p->getLabel());
				}
				break;
			}
			case Opcode::Call:
				os << " ";
				writeHtml(os, cast<CallNode>(n)->getCallee());
				break;
			case Opcode::Region:
				if(cast<RegionNode>(n)->isLoopHeader())
					os << " (loop)";
				break;
			default:
				break;
			}
		}

		U32 outCount(const Node* n) {
			const Type* t = n->getType();
			return t->isTuple() ? t->getTupleElementCount() : 1;
		}

		// which output element an edge leaves from
		U32 outElem(const Node* from, const Node* to) {
			if(from->getType()->isTuple() && to->getOpcode() == Opcode::Proj)
				return cast<ProjNode>(to)->getIndex();
			return 0;
		}

		// a labeled proj names the tuple element it extracts
		String outName(const Node* n, U32 e) {
			const Type* t = n->getType();
			if(!t->isTuple())
				return t->str();
			for(const Node* u : n->getUsers())
				if(u->getOpcode() == Opcode::Proj) {
					const auto* p = cast<ProjNode>(u);
					if(p->getIndex() == e && *p->getLabel())
						return p->getLabel();
				}
			return t->getTupleElement(e)->str();
		}

		void writeNode(std::ostream& os, U32 fnIndex, const Node* n,
									 const List<U32>& useCount) {
			U32 in = n->getInputCount();
			U32 out = outCount(n);
			U32 total = 0;
			for(U32 e = 0; e < out; ++e)
				total += std::max(useCount[e], 1u);
			U32 cols = lcm(in ? in : 1, total);

			os << "    ";
			writeId(os, fnIndex, n);
			os << " [label=<<table border=\"0\" cellborder=\"1\" cellspacing=\"0\" "
						"cellpadding=\"3\">";
			if(in) {
				os << "<tr>";
				for(U32 i = 0; i < in; ++i) {
					os << "<td port=\"i" << i << "\" colspan=\"" << cols / in
						 << "\" valign=\"bottom\">&#160;";
					writeHtml(os, portName(n, i));
					os << "&#160;</td>";
				}
				os << "</tr>";
			}
			os << "<tr><td colspan=\"" << cols << "\">&#160;";
			writeTitle(os, n);
			os << "&#160;</td></tr><tr>";
			U32 port = 0;
			for(U32 e = 0; e < out; ++e) {
				String name = outName(n, e);
				for(U32 k = 0, uk = std::max(useCount[e], 1u); k < uk; ++k) {
					os << "<td port=\"o" << port++ << "\" colspan=\"" << cols / total
						 << "\" valign=\"bottom\">&#160;";
					writeHtml(os, name);
					os << "&#160;</td>";
				}
			}
			os << "</tr></table>>];\n";
		}

		// a proj edge carries whatever the proj extracts, not the tuple
		const Type* carriedType(const Node* from, const Node* to) {
			const Type* t = from->getType();
			if(t->isTuple() && to->getOpcode() == Opcode::Proj)
				t = to->getType();
			return t;
		}

		const C8* edgeStyle(const Type* t) {
			if(t->isControl())
				return "color=\"#cc0000\", weight=4"; // control spine
			if(t->isMemory())
				return "color=\"#0000ff\""; // memory thread
			return "color=\"#000000\""; // data
		}

		void emitFunction(std::ostream& os, U32 index, const Function& fn) {
			// uses per output element, next assigns each edge its own port cell
			Map<const Node*, List<U32>> uses, next;
			for(const Node* n : fn) {
				uses[n].assign(outCount(n), 0);
				next[n].assign(outCount(n), 0);
			}
			for(const Node* n : fn)
				for(U32 i = 0, e = n->getInputCount(); i < e; ++i)
					if(const Node* in = n->getInput(i))
						++uses[in][outElem(in, n)];

			os << "  subgraph cluster_" << index << " {\n    label=\"";
			writeEscaped(os, fn.getName());
			os << "\";\n    penwidth=0;\n";
			for(const Node* n : fn)
				writeNode(os, index, n, uses[n]);
			os << "\n";
			for(const Node* n : fn) {
				for(U32 i = 0, e = n->getInputCount(); i < e; ++i) {
					const Node* in = n->getInput(i);
					if(!in)
						continue;
					U32 elem = outElem(in, n);
					const List<U32>& cnt = uses[in];
					U32 flat = next[in][elem]++;
					for(U32 j = 0; j < elem; ++j)
						flat += std::max(cnt[j], 1u);
					os << "    ";
					writeId(os, index, in);
					os << ":o" << flat << ":s -> ";
					writeId(os, index, n);
					os << ":i" << i << ":n [" << edgeStyle(carriedType(in, n)) << "];\n";
				}
			}
			os << "  }\n";
		}
	} // namespace

	GraphEmitterPass::GraphEmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* GraphEmitterPass::name() const { return "graph-emitter"; }

	B32 GraphEmitterPass::run(Module& module, const TargetInfo&) {
		*os << "digraph rat {\n"
					 "  rankdir=TB;\n"
					 "  graph [fontsize=11, labeljust=l];\n"
					 "  node [shape=plain, fontsize=10];\n"
					 "  edge [fontsize=9, arrowsize=0.8];\n\n";
		U32 index = 0;
		for(const Function* fn : module)
			emitFunction(*os, index++, *fn);
		*os << "}\n";
		return false;
	}
} // namespace rat
