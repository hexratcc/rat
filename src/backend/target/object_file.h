#ifndef RAT_TARGET_OBJECTFILE_H
#define RAT_TARGET_OBJECTFILE_H

#include "core.h"

#include "target/target.h"

#include <iosfwd>

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

		static constexpr U32 kSections = 4;
		static constexpr U32 kByteSections = 3;

		using RelBuckets = List<const Rel*>[kSections];
		void partitionRelocs(RelBuckets buckets) const;
		U32 elfSymtab(List<U8>& symtab, List<U8>& strtab, List<U32>& remap) const;
		static List<U8> elfRela(const List<const Rel*>& bucket, const List<U32>& remap);
		List<U8> coffSymtab(const RelBuckets rels, List<U8>& strtab, List<U32>& index) const;

		ObjectFormat format;
		List<U8> raw[kByteSections];
		U32 bssSize = 0;
		U32 secAlign[kSections] = {16, 16, 16, 16};

		List<Sym> syms;
		Map<String, U32> symByName;
		List<Rel> relocs;
	};

	UniquePtr<ObjectFile> createObjectFile(OS os);

	namespace detail {
		U32 appendName(List<U8>& tab, const C8* n);
		U64 place(U64& off, U64 size, U64 a);
		void emitAt(List<U8>& out, U64 target, const List<U8>& blob);
		void writeElfHeader(List<U8>& out, U64 offSh);
		void coffName(List<U8>& out, List<U8>& strtab, const String& n);
	} // namespace detail
} // namespace rat

#endif
