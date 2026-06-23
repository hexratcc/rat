#ifndef RAT_TARGET_X86ELF_H
#define RAT_TARGET_X86ELF_H

#include "Core.h"

namespace rat {
	enum class ElfReloc : U32 {
		Pc32 = 2,  // R_X86_64_PC32  (S + A - P), used by call/lea(rip)
		Plt32 = 4, // R_X86_64_PLT32 (L + A - P), call to a function via PLT
		Abs64 = 1, // R_X86_64_64    (S + A), absolute 64-bit address
	};

	struct ElfObject {
		enum Section { Text, Rodata, Data, Bss };

		ElfObject();

		U32 append(Section sec, const U8* bytes, U32 len);
		U32 appendZero(Section sec, U32 len);
		U32 align(Section sec, U32 align);
		void defineSymbol(const String& name, Section sec, U32 offset, B32 global,
											B32 isFunc);
		void addReloc(Section sec, U32 offset, const String& symbol, ElfReloc kind,
									I64 addend);
		void write(std::ostream& os);

	private:
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
			ElfReloc kind;
			I64 addend;
		};

		U32 symbolIndex(const String& name);
		U32 sectionSize(Section sec) const;

		List<U8> text;
		List<U8> rodata;
		List<U8> data;
		U32 bssSize = 0;

		List<Sym> syms;
		Map<String, U32> symByName;
		List<Rel> relocs;

		List<U8>& bytesOf(Section sec);
	};
} // namespace rat

#endif
