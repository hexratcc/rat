#ifndef RAT_PASS_VERIFY_H
#define RAT_PASS_VERIFY_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Module;
	struct Node;

	B32 verify(const Function& fn, List<String>& errors);
	B32 verify(const Module& module, List<String>& errors);
	B32 verify(const Function& fn, std::ostream& os);
	B32 verify(const Module& module, std::ostream& os);

	struct VerifyPass : Pass {
		explicit VerifyPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;

		struct FunctionVerifier {
			const Function& fn;
			List<String>& errs;
			Set<const Node*> inFn;
			U32 startErrs;

			FunctionVerifier(const Function& fn, List<String>& e);

			static String vref(const Node* n);

			B32 run();
			void err(const Node* n, const String& msg);
			B32 check(B32 cond, const Node* n, const String& msg);
			static B32 isCtrl(const Node* n);
			static B32 isMem(const Node* n);
			static B32 isData(const Node* n);
			B32 checkArity(const Node* n);
			void checkEdges(Node* n);
			void checkNode(Node* n);
			void checkStopReturns();
		};
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
