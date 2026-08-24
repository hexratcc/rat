#include "pass/emit/text_emitter.h"

#include "ir/function.h"
#include "ir/module.h"
#include "ir/node.h"

namespace rat {
	void TextEmitterPass::comment(std::ostream& os, const C8* text) { os << Green << text << Reset; }

	String TextEmitterPass::quoteBytes(const List<U8>& bytes) {
		// raw bytes as a quoted string: printable ASCII verbatim, everything
		// else (and " and \) as a \HH hex escape, so the form round-trips
		static const C8* hex = "0123456789abcdef";
		String out = "\"";
		for(U8 b : bytes) {
			if(b >= 0x20 && b <= 0x7e && b != '"' && b != '\\') {
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

	void TextEmitterPass::ref(std::ostream& os, const Node* node) {
		if(!node) {
			os << "<null>";
			return;
		}
		U32 id = node->getId();
		os << TempColors[id % TempColorCount] << 'v' << id << Reset;
	}

	void TextEmitterPass::emitOperands(const Node* node) {
		for(U32 i = 0; i < node->getInputCount(); ++i) {
			*os << (i ? ", " : " ");
			ref(*os, node->getInput(i));
		}
	}

	void TextEmitterPass::emitNode(const Node* node) {
		*os << "  ";
		ref(*os, node);
		*os << " = " << node->getMnemonic() << " : ";
		node->getType()->print(*os);

		switch(node->getOpcode()) {
		case Opcode::Constant:
			*os << Green << "  " << cast<ConstantNode>(node)->getValue() << Reset;
			break;
		case Opcode::Proj: {
			const auto* proj = cast<ProjNode>(node);
			*os << Green << "  #" << proj->getIndex() << Reset;
			if(*proj->getLabel())
				*os << Green << " \"" << proj->getLabel() << "\"" << Reset;
			comment(*os, " of ");
			ref(*os, proj->getProducer());
			return;
		}
		case Opcode::Call:
			*os << Green << "  \"" << cast<CallNode>(node)->getCallee() << "\"" << Reset;
			break;
		case Opcode::Asm: {
			const String& text = cast<AsmNode>(node)->getText();
			List<U8> bytes(text.begin(), text.end());
			*os << Green << "  " << quoteBytes(bytes) << Reset;
			break;
		}
		case Opcode::Global:
			*os << Green << "  \"" << cast<GlobalNode>(node)->getSymbol() << "\"" << Reset;
			break;
		case Opcode::Alloc:
			*os << Green << "  ";
			cast<AllocNode>(node)->getAllocType()->print(*os);
			*os << Reset;
			break;
		case Opcode::Region:
			if(cast<RegionNode>(node)->isLoopHeader())
				comment(*os, "  loop");
			break;
		case Opcode::Extract:
			*os << Green << "  #" << cast<ExtractNode>(node)->getLane() << Reset;
			break;
		case Opcode::Shuffle:
			*os << Green << "  #" << (U32)cast<ShuffleNode>(node)->getSelector() << Reset;
			break;
		default:
			break;
		}

		emitOperands(node);
	}

	void TextEmitterPass::emitFunction(const Function& fn) {
		*os << "func " << fn.getName() << "(";
		for(U32 i = 0; i < fn.getParamCount(); ++i) {
			if(i)
				*os << ", ";
			fn.getParamType(i)->print(*os);
		}
		*os << ") -> ";
		if(fn.returnsValue())
			fn.getReturnType()->print(*os);
		else
			*os << "void";
		if(fn.isNoInline())
			*os << " noinline";
		*os << " {\n";

		for(const Node* node : fn) {
			emitNode(node);
			*os << "\n";
		}

		*os << "}\n";
	}

	void TextEmitterPass::emitModule(const Module& module) {
		B32 any = false;
		for(const Global* g : module.globals()) {
			*os << (g->isConstant() ? "const " : "var ") << g->getName() << " : ";
			g->getType()->print(*os);
			*os << " = " << quoteBytes(g->getInit()) << "\n";
			any = true;
		}

		B32 first = true;
		for(const Function* fn : module) {
			if(!first || any)
				*os << "\n";
			first = false;
			emitFunction(*fn);
		}
	}

	TextEmitterPass::TextEmitterPass(std::ostream& os)
	: os(&os) {}

	const C8* TextEmitterPass::name() const { return "text-emitter"; }

	B32 TextEmitterPass::run(Module& module, const TargetInfo&) {
		emitModule(module);
		return false;
	}
} // namespace rat
