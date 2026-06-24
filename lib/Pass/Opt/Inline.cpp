// function inlining: replace a call to a small, non-recursive function with a
// clone of the callee's body, splicing the callee's control and memory edges
// into the caller and merging its returns at the call's continuation
//
// references:
// - C. Click and M. Paleczny, "A Simple Graph-Based Intermediate
//   Representation", ACM SIGPLAN Workshop on IRs, 1995
// - S. Muchnick, "Advanced Compiler Design and Implementation", 1997, ch. 15

#include "Pass/Opt/Inline.h"

#include "IR/Function.h"
#include "IR/Module.h"
#include "IR/Node.h"
#include "IR/Type.h"

namespace rat {
	namespace detail {
		constexpr U32 kInlineNodeBudget = 64;
		constexpr U32 kMaxInlinesPerFunction = 256;

		Function* findFunction(Module& m, const String& name) {
			for (Function* f : m)
				if (f->getName() == name)
					return f;
			return nullptr;
		}

		U32 nodeCount(const Function& fn) {
			U32 n = 0;
			for (Node* it : fn) {
				(void)it;
				++n;
			}
			return n;
		}

		B32 hasReturn(const Function& fn) {
			for (Node* n : fn)
				if (n->getOpcode() == Opcode::Return)
					return true;
			return false;
		}

		B32 isStartProj(const Function& callee, Node* n) {
			ProjNode* p = dyn_cast<ProjNode>(n);
			return p && p->getProducer() == callee.getStart();
		}

		B32 inlineCallSite(Function& caller, CallNode* call, Function& callee) {
			Map<Node*, Node*> map;
			for (Node* u : callee.getStart()->getUsers()) {
				ProjNode* p = dyn_cast<ProjNode>(u);
				if (!p)
					continue;
				U32 idx = p->getIndex();
				if (idx == StartNode::controlProjIndex())
					map[p] = call->getControl();
				else if (idx == StartNode::memoryProjIndex())
					map[p] = call->getMemory();
				else {
					U32 a = idx - StartNode::paramProjIndex(0);
					if (a >= call->getArgCount())
						return false; // signature mismatch
					map[p] = call->getArg(a);
				}
			}

			// shallow-clone every body node
			for (Node* n : callee) {
				if (n == callee.getStart() || n == callee.getStop())
					continue;
				if (n->getOpcode() == Opcode::Return)
					continue;
				if (map.count(n))
					continue;
				Node* c = cloneShell(caller, n);
				if (!c)
					return false;
				map[n] = c;
			}

			auto resolve = [&](Node* n) -> Node* {
				auto it = map.find(n);
				return it == map.end() ? n : it->second;
			};

			// wire each clone's inputs through the map
			for (auto& kv : map) {
				Node* orig = kv.first;
				if (isStartProj(callee, orig))
					continue; // seeded with an external value; nothing to wire
				Node* clone = kv.second;
				for (U32 i = 0, e = orig->getInputCount(); i < e; ++i)
					clone->setInput(i, resolve(orig->getInput(i)));
			}

			// collect the callee's mapped return triples and merge them
			List<Node*> ctrls, mems, vals;
			for (Node* n : callee) {
				ReturnNode* r = dyn_cast<ReturnNode>(n);
				if (!r)
					continue;
				ctrls.push_back(resolve(r->getControl()));
				mems.push_back(resolve(r->getMemory()));
				if (r->hasValue())
					vals.push_back(resolve(r->getValue()));
			}
			if (ctrls.empty())
				return false; // callee never returns

			Node* mergedCtrl = nullptr;
			Node* mergedMem = nullptr;
			Node* mergedVal = nullptr;
			if (ctrls.size() == 1) {
				mergedCtrl = ctrls[0];
				mergedMem = mems[0];
				mergedVal = vals.empty() ? nullptr : vals[0];
			} else {
				RegionNode* reg = caller.create<RegionNode>(caller.ctrlTy(), ctrls);
				mergedCtrl = reg;
				List<Node*> memIns{reg};
				for (Node* m : mems)
					memIns.push_back(m);
				mergedMem = caller.create<PhiNode>(caller.memTy(), memIns);
				if (callee.returnsValue() && vals.size() == ctrls.size()) {
					List<Node*> valIns{reg};
					for (Node* v : vals)
						valIns.push_back(v);
					mergedVal = caller.create<PhiNode>(callee.getReturnType(), valIns);
				}
			}

			// redirect the call's projections onto the merged values, then drop the
			// projections and the call itself
			List<Node*> projs;
			for (Node* u : call->getUsers())
				if (isa<ProjNode>(u))
					projs.push_back(u);
			for (Node* pn : projs) {
				U32 idx = cast<ProjNode>(pn)->getIndex();
				Node* repl = nullptr;
				if (idx == CallNode::controlProjIndex())
					repl = mergedCtrl;
				else if (idx == CallNode::memoryProjIndex())
					repl = mergedMem;
				else if (idx == CallNode::valueProjIndex())
					repl = mergedVal;
				if (repl)
					pn->replaceAllUsesWith(repl);
				caller.removeNode(pn);
			}
			caller.removeNode(call);
			return true;
		}

		B32 shouldInline(const Function& caller, CallNode* call, Function* callee) {
			if (!callee || callee == &caller)
				return false; // missing or directly recursive
			if (!hasReturn(*callee))
				return false;
			if (call->returnsValue() != callee->returnsValue())
				return false;
			if (call->getArgCount() != callee->getParamCount())
				return false;
			return nodeCount(*callee) <= kInlineNodeBudget;
		}

		U32 inlineInto(Function& caller, Module& m) {
			U32 count = 0;
			B32 changed = true;
			while (changed && count < kMaxInlinesPerFunction) {
				changed = false;
				List<CallNode*> calls;
				for (Node* n : caller)
					if (CallNode* c = dyn_cast<CallNode>(n))
						calls.push_back(c);
				for (CallNode* c : calls) {
					Function* callee = findFunction(m, c->getCallee());
					if (!shouldInline(caller, c, callee))
						continue;
					if (inlineCallSite(caller, c, *callee)) {
						++count;
						changed = true;
						break;
					}
				}
			}
			if (count)
				caller.eliminateDeadNodes();
			return count;
		}
	} // namespace detail
	using namespace detail;

	U32 inlineCalls(Module& module) {
		U32 count = 0;
		for (Function* fn : module)
			count += inlineInto(*fn, module);
		return count;
	}

	const C8* InlinePass::name() const { return "inline"; }

	B32 InlinePass::run(Module& module) { return inlineCalls(module) != 0; }
} // namespace rat
