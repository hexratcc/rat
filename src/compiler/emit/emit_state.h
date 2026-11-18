#ifndef RAT_CC_EMIT_STATE_H
#define RAT_CC_EMIT_STATE_H

#include "parse/ast.h"

#include "ir/function.h"
#include "ir/module.h"

#include <map>

namespace rat::cc {
	struct TokenStream;
	using Block = Function::Block;

	struct Local {
		enum class Kind : U8 {
			Var,
			Mem,
			Function // a block-scope function declaration, see lookupSymbol
		};
		Kind kind = Kind::Var;
		Function::Var var = 0;
		Node* addr = nullptr;
		CType type;
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
	};

	using Scopes = ScopeMap<Local>;

	struct LoopFrame {
		Block* breakBlock = nullptr;
		Block* continueBlock = nullptr;
		B32 exitReachable = false;
		B32 isSwitch = false;
		Node* sp = nullptr;
	};

	struct Label {
		Block* block = nullptr;
		Node* sp = nullptr;
	};

	struct FunctionState {
		void reset();

		CType returnType;
		Node* sretSlot = nullptr;
		Node* sp = nullptr;
		B32 sawAlloca = false;
		Set<String> addrTaken;
		List<Pair<Function::Var, U32>> restrictVars;
		Map<String, Label> labels;
		List<LoopFrame> loops;
		List<Map<const Stmt*, Block*>> switches;
		Scopes scopes;
		Map<const ArrayType*, Node*> vlaDims; // bound of a VLA dim, taken once at its declaration
	};

	struct Diagnostics {
		Failed fail(const String& m);

		B32 failed = false;
		String message;
		List<String> warnings;
		U32 offset = 0;
		String function;
	};

	// moves diag aside for a dry run and puts it back when it ends
	struct DryRunDiag {
		explicit DryRunDiag(Diagnostics& current);
		~DryRunDiag();

		Diagnostics& diag;
		Diagnostics saved;
	};

	struct GlobalVariable {
		CType type;
	};
	struct FunctionSignature {
		const FuncType* type = nullptr;
		B32 noInline = false;
		U32 align = 0;
	};

	// what a name refers to, local before global before function
	struct Symbol {
		enum class Kind : U8 {
			None,
			Local,
			Global,
			Function
		};
		Kind kind = Kind::None;
		const Local* local = nullptr;
		CType type; // a function is its pointer type
	};

	struct Symbols {
		const String& resolveAlias(const String& name) const;

		Map<String, FunctionSignature> functions;
		Map<String, const FuncType*> implicitFunctions; // unprototyped, by name
		Map<String, GlobalVariable> globals;
		Map<String, String> aliases;
		Map<U32, StructType*> complexTypes;
	};

	// -g state, inactive while info is null
	struct DebugState {
		const TokenStream* toks = nullptr;
		DebugInfo* info = nullptr;
		B32 keepLocals = false;					// every named local in a frame slot, so -O0 code is inspectable
		B32 describeVars = false;				// types and variables, else lines only
		U32 scope = 0;									// current DebugBody scope
		const Stmt* funcBody = nullptr; // its scope is 0
		Map<U16, U32> files;						// token file index to DebugInfo file
		Map<String, DebugType*> bases;	// by name
		std::map<Pair<DebugType::Kind, const void*>, DebugType*> types; // records, named and wrappers
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
