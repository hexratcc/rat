#ifndef RAT_PASS_EMIT_GRAPHEMITTER_H
#define RAT_PASS_EMIT_GRAPHEMITTER_H

#include "Core.h"
#include "Support/Pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;

	struct GraphEmitterPass : Pass {
		explicit GraphEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;
	private:
		void emitModule(const Module& module);
		void emitFunctionBody(const Function& fn);

		String getNodeId(const Function& fn, const Node* n);
		void getStyle(const Node* n, String& label, String& attrs);
		const C8* getEdgeStyle(const Node* producer);
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
