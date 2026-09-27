#ifndef RAT_CC_EMIT_STATE_H
#define RAT_CC_EMIT_STATE_H

#include "parse/ast.h"

#include "ir/function.h"
#include "ir/module.h"

namespace rat::cc {
	using Block = Function::Block;

	struct Local {
		enum class Kind : U8 { Var, Mem };
		Kind kind = Kind::Var;
		Function::Var var = 0;
		Node* addr = nullptr;
		CType type;
		B32 isArray = false;
		U32 count = 0;
		Node* lengthNode = nullptr;						// runtime byte size for a VLA
		const String* staticSymbol = nullptr; // symbol backing a function-local static

		B32 inMem() const { return kind == Kind::Mem; }
		static Local inVar(Function::Var v, CType t) {
			Local l;
			l.var = v;
			l.type = t;
			return l;
		}
		static Local mem(Node* addr, CType t) {
			Local l;
			l.kind = Kind::Mem;
			l.addr = addr;
			l.type = t;
			return l;
		}
		static Local memArray(Node* addr, CType t, U32 count = 0) {
			Local l = mem(addr, t);
			l.isArray = true;
			l.count = count;
			return l;
		}
	};

	// flat scope stack: one table + undo log rolled back on pop
	struct Scopes {
		void push();
		void pop();
		void declare(const String& name, Local local);
		B32 lookup(const String& name, Local& out) const;
		void clear();
	private:
		struct Undo {
			const String* name; // table key, stable across rehash
			Local prev;
			B32 hadPrev;
		};
		Map<String, Local> table;
		List<Undo> undo;
		List<U32> marks;
	};

	struct LoopFrame {
		Block* breakBlock = nullptr;
		Block* continueBlock = nullptr;
		B32 exitReachable = false;
		B32 isSwitch = false;
		Node* sp = nullptr;
	};

	struct FunctionState {
		void reset();

		CType returnType;
		Node* sretSlot = nullptr;
		Node* sp = nullptr;
		Map<String, Node*> labelSp;
		B32 sawAlloca = false;
		Set<String> addrTaken;
		Map<String, Block*> labelBlocks;
		List<LoopFrame> loops;
		List<Map<const Stmt*, Block*>> switches;
		Scopes scopes;
	};

	struct Diagnostics {
		void fail(const String& m);

		B32 failed = false;
		String message;
		List<String> warnings;
		U32 offset = 0;
		String function;
	};

	struct GlobalVariable {
		CType type;
		B32 isArray = false;
		U32 count = 0;
	};
	struct FunctionSignature {
		CType returnType;
		List<CType> params;
		B32 isVarArgs = false;
		B32 unprototyped = false;
		B32 noInline = false;
		U32 align = 0;
	};

	struct Symbols {
		const String& resolveAlias(const String& name) const;

		Map<String, FunctionSignature> functions;
		Set<String> implicitFunctions;
		Map<String, GlobalVariable> globals;
		Map<String, String> aliases;
		Map<U32, StructType*> complexTypes;
	};

	struct GlobalData {
		String nextName(const C8* prefix);
		void setReloc(U32 off, const String& sym, I64 add);

		List<Reloc> relocs;
		U32 flexCount = 0;
		U32 nameCounter = 0;
		U32 staticCounter = 0;
		Map<String, String> stringPool; // string-literal bytes -> interned symbol
	};
} // namespace rat::cc

#endif
