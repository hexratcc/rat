#ifndef RAT_TARGET_OBJECTFILE_H
#define RAT_TARGET_OBJECTFILE_H

#include "Core.h"

#include "Target/Target.h"

namespace rat {
	enum class RelocKind : U32 {
		Abs64 = 1, // absolute 64-bit address       (S + A)
		Pc32 = 2,	 // 32-bit pc-relative            (S + A - P), used by lea(rip)
		Plt32 = 4, // 32-bit pc-relative call       (L + A - P), call to a function
	};

	struct ObjectFile {
		enum Section { Text, Rodata, Data, Bss };

		ObjectFile();
		virtual ~ObjectFile() = default;

		U32 append(Section sec, const U8* bytes, U32 len);
		U32 appendZero(Section sec, U32 len);
		U32 align(Section sec, U32 align);
		void defineSymbol(const String& name, Section sec, U32 offset, B32 global, B32 isFunc);
		void addReloc(Section sec, U32 offset, const String& symbol, RelocKind kind, I64 addend);
		virtual void write(std::ostream& os) = 0;
	protected:
		struct Sym {
			String name;
			Section sec;
			U32 offset;
			B32 defined;
			B32 global;
			B32 isFunc;
		};
		struct Rel {
			Section sec;
			U32 offset;
			U32 symIndex;
			RelocKind kind;
			I64 addend;
		};

		static void put8(List<U8>& b, U8 v);
		static void put16(List<U8>& b, U16 v);
		static void put32(List<U8>& b, U32 v);
		static void put64(List<U8>& b, U64 v);

		U32 symbolIndex(const String& name);
		U32 sectionSize(Section sec) const;
		List<U8>& bytesOf(Section sec);

		List<U8> text;
		List<U8> rodata;
		List<U8> data;
		U32 bssSize = 0;

		List<Sym> syms;
		Map<String, U32> symByName;
		List<Rel> relocs;
	};

	UniquePtr<ObjectFile> createObjectFile(OS os);
} // namespace rat

#endif
