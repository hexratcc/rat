#ifndef RAT_TARGET_OBJECTFILE_H
#define RAT_TARGET_OBJECTFILE_H

#include "core.h"

#include "target/target.h"

namespace rat {
	enum class RelocKind : U32 {
		Abs64 = 1, // absolute 64-bit address       (S + A)
		Pc32 = 2,	 // 32-bit pc-relative            (S + A - P), used by lea(rip)
		Plt32 = 4, // 32-bit pc-relative call       (L + A - P), call to a function
	};

	enum class ObjectFormat : U32 { Coff, Elf };

	constexpr ObjectFormat objectFormatFor(OS os) {
		return os == OS::Windows ? ObjectFormat::Coff : ObjectFormat::Elf;
	}

	struct ObjectFile {
		enum Section { Text, Rodata, Data, Bss };

		explicit ObjectFile(ObjectFormat fmt = ObjectFormat::Elf);

		U32 append(Section sec, const U8* bytes, U32 len);
		U32 appendZero(Section sec, U32 len);
		U32 align(Section sec, U32 align);
		U32 sectionAlign(Section sec) const { return secAlign[sec]; }
		void defineSymbol(const String& name, Section sec, U32 offset, B32 global, B32 isFunc);
		B32 defineAlias(const String& name, const String& target, B32 global);
		void addReloc(Section sec, U32 offset, const String& symbol, RelocKind kind, I64 addend);

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
			RelocKind kind;
			I64 addend;
		};

		void writeCoff(std::ostream& os);
		void writeElf(std::ostream& os);

		U32 symbolIndex(const String& name);
		U32 sectionSize(Section sec) const;
		List<U8>& bytesOf(Section sec);
		const List<U8>& bytesOf(Section sec) const;

		static constexpr U32 kSections = 4;
		static constexpr U32 kByteSections = 3;

		using RelBuckets = List<const Rel*>[kSections];
		void partitionRelocs(RelBuckets buckets) const;

		ObjectFormat format;
		List<U8> raw[kByteSections];
		U32 bssSize = 0;
		U32 secAlign[kSections] = {16, 16, 16, 16};

		List<Sym> syms;
		Map<String, U32> symByName;
		List<Rel> relocs;
	};

	UniquePtr<ObjectFile> createObjectFile(OS os);
} // namespace rat

#endif
