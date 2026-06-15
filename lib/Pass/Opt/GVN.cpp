// global value numbering: hash-cons congruent nodes so equal computations
// share a single node
//
// references:
// - B. Alpern, M. Wegman and F. K. Zadeck, "Detecting Equality of Variables
//   in Programs", POPL, 1988
// - C. Click, "Global Code Motion / Global Value Numbering", PLDI, 1995

#include "Pass/Opt/GVN.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace rat {
	namespace {
		B32 isPureValue(Node* n) {
			Opcode op = n->getOpcode();
			return op == Opcode::Constant || isBinaryOpcode(op) ||
						 isUnaryOpcode(op) || isCompareOpcode(op) || isConvertOpcode(op);
		}

		String signatureOf(Node* n) {
			String key = std::to_string((U32)n->getOpcode()) + "|" +
									 std::to_string((uintptr_t)n->getType()) + "|";
			if (n->getOpcode() == Opcode::Constant)
				return key + "c" +
							 std::to_string(static_cast<ConstantNode*>(n)->getValue());

			List<U32> ops;
			ops.reserve(n->getInputCount());
			for (U32 i = 0, e = n->getInputCount(); i < e; ++i)
				ops.push_back(n->getInput(i)->getId());
			if (n->isCommutative())
				std::sort(ops.begin(), ops.end());
			for (U32 id : ops)
				key += std::to_string(id) + ",";
			return key;
		}
	} // namespace

	U32 gvn(Function& fn) {
		U32 removed = 0;

		B32 changed = true;
		while (changed) {
			changed = false;
			Map<String, Node*> table;
			for (Node* n : fn) {
				if (!isPureValue(n) || !n->hasUsers())
					continue;
				String sig = signatureOf(n);
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

	const char* GVNPass::name() const { return "gvn"; }

	B32 GVNPass::run(Module& module) {
		U32 removed = 0;
		for (Function* fn : module)
			removed += gvn(*fn);
		return removed != 0;
	}
} // namespace rat
