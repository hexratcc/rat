#ifndef RAT_PASS_EMIT_TEXTEMITTER_H
#define RAT_PASS_EMIT_TEXTEMITTER_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;

	struct TextEmitterPass : Pass {
		static constexpr const C8* Reset = "\033[0m";
		static constexpr const C8* Green = "\033[32m";
		static constexpr const C8* TempColors[] = {
			"\033[31m", "\033[33m", "\033[34m", "\033[35m", "\033[36m",
			"\033[91m", "\033[93m", "\033[94m", "\033[95m", "\033[96m",
		};
		static constexpr U32 TempColorCount = sizeof(TempColors) / sizeof(TempColors[0]);

		explicit TextEmitterPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module) override;
	private:
		void emitModule(const Module& module);
		void emitFunction(const Function& fn);
		void emitNode(const Node* node);
		void emitOperands(const Node* node);

		String comment(const String& text);
		String quoteBytes(const List<U8>& bytes);
		String ref(const Node* node);
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
