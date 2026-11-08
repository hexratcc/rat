// target-agnostic source-level debug description
#ifndef RAT_IR_DEBUGINFO_H
#define RAT_IR_DEBUGINFO_H

#include "core.h"

namespace rat {
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
			Function
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
		DebugType* elem = nullptr; // pointee, element or return type, null = void
		U64 count = 0;						 // array length, 0 = unknown
		List<Member> members;
		List<DebugType*> params;
		B32 variadic = false;
		B32 incomplete = false; // declared struct without a body
	};

	// a source variable
	struct DebugVar {
		String name;
		DebugType* type;
		SrcLoc decl;
		String symbol; // static storage, else empty
	};

	// a source function, outlives its IR function so inlined copies can still name it
	struct DebugFunc {
		String name;
		SrcLoc decl;
		DebugType* ret = nullptr;
		B32 external = false;
		B32 variadic = false;
	};

	struct DebugLoc {
		SrcLoc src;
	};

	// debug state of one IR function
	struct DebugBody {
		DebugFunc* func;
		List<DebugLoc> locs = List<DebugLoc>(1); // 0 = no location

		U32 addLoc(SrcLoc src);
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
