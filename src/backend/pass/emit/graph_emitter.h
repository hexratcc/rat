#ifndef RAT_PASS_EMIT_GRAPHEMITTER_H
#define RAT_PASS_EMIT_GRAPHEMITTER_H

#include "core.h"
#include "pass/pass.h"

namespace rat {
	struct Function;
	struct Node;
	struct Type;

	namespace detail {
		void writeId(std::ostream& os, U32 fnIndex, const Node* n);
		void writeEscaped(std::ostream& os, const String& s);
		void writeHtml(std::ostream& os, const String& s);
		U32 lcm(U32 a, U32 b);
		String portName(const Node* n, U32 i);
		void writeTitle(std::ostream& os, const Node* n);
		U32 outCount(const Node* n);
		U32 outElem(const Node* from, const Node* to);
		String outName(const Node* n, U32 e);
		void writeNode(std::ostream& os, U32 fnIndex, const Node* n, const List<U32>& useCount);
		const Type* carriedType(const Node* from, const Node* to);
		const C8* edgeStyle(const Type* t);
		void emitFunction(std::ostream& os, U32 index, const Function& fn);
	} // namespace detail

	struct GraphEmitterPass : Pass {
		explicit GraphEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
