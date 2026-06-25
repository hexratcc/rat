#ifndef RAT_PASS_VERIFY_H
#define RAT_PASS_VERIFY_H

#include "Core.h"
#include "Pass/Pass.h"

namespace rat {
	struct Function;
	struct Module;

	B32 verify(const Function& fn, List<String>& errors);
	B32 verify(const Module& module, List<String>& errors);
	B32 verify(const Function& fn, std::ostream& os);
	B32 verify(const Module& module, std::ostream& os);

	struct VerifyPass : Pass {
		explicit VerifyPass(std::ostream& os);

		const C8* name() const override;
		B32 run(Module& module) override;
	private:
		std::ostream* os;
	};
} // namespace rat

#endif
