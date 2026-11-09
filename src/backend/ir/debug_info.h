// target-agnostic source-level debug description
#ifndef RAT_IR_DEBUGINFO_H
#define RAT_IR_DEBUGINFO_H

#include "core.h"

namespace rat {
	struct AllocNode;

	struct SrcLoc {
		U32 file = 0; // DebugInfo::files
		U32 line = 0;
		U32 col = 0;
	};

	struct DebugType {
		enum class Kind : U8 {
			Signed,
			Unsigned,
			SignedChar,
			UnsignedChar,
			Bool,
			Float,
			ComplexFloat,
			Pointer,
			Struct,
			Union,
			Array,
			Function,
			Typedef,
			Enum,
			Const,
			Volatile
		};
		struct Member {
			String name;
			DebugType* type;
			U32 offset;			 // bytes
			U32 bitSize = 0; // 0 = not a bitfield
			U32 bitOffset = 0;
		};

		Kind kind;
		String name;
		U32 size = 0;
		DebugType* elem = nullptr; // pointee, element, return, aliased or underlying type, null = void
		U64 count = 0;						 // array length, 0 = unknown
		List<Member> members;
		List<DebugType*> params;
		B32 variadic = false;
		B32 incomplete = false; // declared struct without a body
		List<Pair<String, I64>> enumerators;
		SrcLoc decl; // typedef and enum
	};

	// a source variable, shared by every copy of its function's code
	struct DebugVar {
		String name;
		DebugType* type;
		SrcLoc decl;
		String symbol; // static storage, else empty
		B32 isParam = false;
	};

	// a source function, outlives its IR function so inlined copies can still name it
	struct DebugFunc {
		String name;
		SrcLoc decl;
		DebugType* ret = nullptr;
		B32 external = false;
		B32 variadic = false;
		List<UniquePtr<DebugVar>> vars; // params first
	};

	// a lexical block, or an inlined call when inlined is set, scope 0 is the function body
	struct DebugScope {
		U32 parent = 0;
		const DebugFunc* inlined = nullptr;
		U32 callLoc = 0;
	};

	struct DebugLoc {
		SrcLoc src;
		U32 scope = 0;
	};

	// one copy of a variable in a function's code
	struct DebugVarInst {
		const DebugVar* var;
		U32 scope = 0;
		AllocNode* slot = nullptr; // frame storage
	};

	// debug state of one IR function, inlining copies it into the caller
	struct DebugBody {
		DebugFunc* func;
		List<DebugLoc> locs = List<DebugLoc>(1);			 // 0 = no location
		List<DebugScope> scopes = List<DebugScope>(1); // 0 = the body
		List<DebugVarInst> vars;

		U32 addLoc(SrcLoc src, U32 scope);
	};

	struct DebugInfo {
		String producer;
		String compDir;
		List<String> files; // files[0] is the primary source
		List<DebugVar> globals;
		List<UniquePtr<DebugFunc>> funcs;
		List<UniquePtr<DebugType>> types;

		DebugType* makeType(DebugType::Kind kind, String name, U32 size);
	};
} // namespace rat

#endif
