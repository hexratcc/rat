// global value numbering: hash-cons congruent nodes so equal computations
// share a single node
//
// references:
// - B. Alpern, M. Wegman and F. K. Zadeck, "Detecting Equality of Variables
//   in Programs", POPL, 1988
// - C. Click, "Global Code Motion / Global Value Numbering", PLDI, 1995

#include "Pass/Opt/GVN.h"

#include "IR/Function.h"
#include "IR/Node.h"

namespace rat {
	namespace detail {
		B32 isPureValue(Node* n) {
			Opcode op = n->getOpcode();
			return op == Opcode::Constant || op == Opcode::Global ||
						 isBinaryOpcode(op) || isUnaryOpcode(op) || isCompareOpcode(op) ||
						 isConvertOpcode(op);
		}
	} // namespace detail
	using namespace detail;

	U32 gvn(Function& fn) {
		U32 removed = 0;

		B32 changed = true;
		while (changed) {
			changed = false;
			Map<String, Node*> table;
			for (Node* n : fn) {
				if (!isPureValue(n) || !n->hasUsers())
					continue;
				String sig = nodeSignature(n);
				auto it = table.find(sig);
				if (it == table.end()) {
					table.emplace(std::move(sig), n);
				} else {
					// n is a duplicate of the earlier representative; redirect its uses
					n->replaceAllUsesWith(it->second);
					++removed;
					changed = true;
				}
			}
		}

		if (removed)
			fn.eliminateDeadNodes();
		return removed;
	}

	const C8* GVNPass::name() const { return "gvn"; }

	U32 GVNPass::runOnFunction(Function& fn) { return gvn(fn); }
} // namespace rat
