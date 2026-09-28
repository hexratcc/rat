#ifndef RAT_PASS_VERIFY_H
#define RAT_PASS_VERIFY_H

#include "core.h"
#include "pass/pass.h"

#include <iosfwd>

namespace rat {
	struct Function;
	struct Node;
	struct Type;

	struct VerifyPass : Pass {
		explicit VerifyPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module, const TargetInfo& target) override;

		struct FunctionVerifier {
			const Function& fn;
			std::ostream& os;
			Set<const Node*> inFn;

			FunctionVerifier(const Function& fn, std::ostream& os);

			static String vref(const Node* n);

			void run();
			void err(const Node* n, const String& msg);
			static B32 isCtrl(const Node* n);
			static B32 isMem(const Node* n);
			static B32 isData(const Node* n);
			static B32 isBool(const Type* t);
			static B32 listsAsUser(const Node* def, const Node* user);
			static B32 listsAsInput(const Node* user, const Node* def);
			B32 checkArity(const Node* n);
			void checkEdges(Node* n);
			void checkNode(Node* n);
			void checkCtrlMem(const Node* n, const C8* what);
			B32 checkTuple(const Node* n, const C8* op, U32 len, const C8* by);
			void checkStart(const Node* n);
			void checkReturn(const Node* n);
			void checkRegion(const Node* n);
			void checkIf(const Node* n);
			void checkSwitch(const Node* n);
			void checkProj(const Node* n);
			void checkPhi(const Node* n);
			void checkLaneOp(const Node* n);
			void checkSelect(const Node* n);
			void checkMemoryOp(const Node* n);
			void checkCall(const Node* n);
			void checkAsm(const Node* n);
			void checkArithmetic(const Node* n);
			void checkBinary(const Node* n);
			void checkUnary(const Node* n);
			void checkConvert(const Node* n);
			void checkStopReturns();
		};
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
