#ifndef RAT_CC_EMIT_H
#define RAT_CC_EMIT_H

#include "Ast.h"

namespace rat {
	struct Module;
	struct Function;
	struct Node;
	struct Type;
} // namespace rat

namespace rat::cc {
	struct Emitter {
		explicit Emitter(Module& module);

		B32 emit(const TransUnit& unit);

		const String& error() const { return errMsg; }

	private:
		B32 emitStmt(Function& fn, const Stmt* stmt);
		Node* emitExpr(Function& fn, const Expr* expr);
		Node* emitAssign(Function& fn, const Expr* expr);

		Node* toBool(Function& fn, Node* value);
		Node* fromBool(Function& fn, Node* value);

		void pushScope();
		void popScope();
		void declare(const String& name, U32 var);
		B32 lookup(const String& name, U32& var) const;

		void fail(const String& msg);

		Module& mod;
		Type* i32 = nullptr;
		B32 failed = false;
		String errMsg;
		List<Map<String, U32>> scopes;
	};
} // namespace rat::cc

#endif
