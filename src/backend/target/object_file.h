#ifndef RAT_TARGET_OBJECTFILE_H
#define RAT_TARGET_OBJECTFILE_H

#include "core.h"

#include "elf_file.h"
#include "target/target.h"

#include <iosfwd>

namespace rat {
	enum class RelocKind : U32 {
		Abs64 = 1,	// absolute 64-bit address       (S + A)
		Pc32 = 2,		// 32-bit pc-relative            (S + A - P), used by lea(rip)
		Plt32 = 4,	// 32-bit pc-relative call       (L + A - P), call to a function
		Abs32 = 10, // 32-bit absolute              (S + A), debug section offsets
	};

	enum class ObjectFormat : U32 {
		Coff,
		Elf
	};

	constexpr ObjectFormat objectFormatFor(OS os) {
		return os == OS::Windows ? ObjectFormat::Coff : ObjectFormat::Elf;
	}

	struct ObjectFile {
		enum Section {
			Text,
			Rodata,
			Data,
			Bss,
			EhFrame,
			DebugAbbrev,
			DebugInfo,
			DebugLine,
			DebugRnglists,
			DebugLoclists,
			SectionCount
		};

		explicit ObjectFile(ObjectFormat fmt = ObjectFormat::Elf);

		U32 append(Section sec, const U8* bytes, U32 len);
		U32 appendZero(Section sec, U32 len);
		U32 align(Section sec, U32 align);
		void defineSymbol(const String& name, Section sec, U32 offset, B32 global, B32 isFunc);
		B32 defineAlias(const String& name, const String& target, B32 global);
		void addReloc(Section sec, U32 offset, const String& symbol, RelocKind kind, I64 addend);
		// a local symbol at the start of sec, named like it, for section-relative relocs
		String sectionSymbol(Section sec);
		U32 sectionSize(Section sec) const;

		void write(std::ostream& os);
	private:
		struct Sym {
			String name;
			Section sec;
			U32 offset;
			B32 defined;
			B32 global;
			B32 isFunc;
			B32 isSection = false;
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
		List<U8>& bytesOf(Section sec);

		using RelBuckets = List<const Rel*>[SectionCount];
		void partitionRelocs(RelBuckets buckets) const;
		U32
		elfSymtab(List<U8>& symtab, List<U8>& strtab, List<U32>& remap, const List<U32>& shIndex) const;
		static List<U8> elfRela(const List<const Rel*>& bucket, const List<U32>& remap);
		List<U8> coffSymtab(const RelBuckets rels, List<U8>& strtab, List<U32>& index) const;

		ObjectFormat format;
		List<U8> raw[SectionCount]; // bss slot unused
		U32 bssSize = 0;
		U32 requestedAlign[SectionCount] = {}; // by align(), over the kElfSections default

		List<Sym> syms;
		Map<String, U32> symByName;
		List<Rel> relocs;
	};

	UniquePtr<ObjectFile> createObjectFile(OS os);

	namespace detail {
		// clang-format off
		constexpr elf::SectionInfo kElfSections[] = {
				// name             type                    flags                                align entsize
				{".text",           elf::SHT_PROGBITS,      elf::SHF_ALLOC | elf::SHF_EXECINSTR, 16,   0},
				{".rodata",         elf::SHT_PROGBITS,      elf::SHF_ALLOC,                      16,   0},
				{".data",           elf::SHT_PROGBITS,      elf::SHF_ALLOC | elf::SHF_WRITE,     16,   0},
				{".bss",            elf::SHT_NOBITS,        elf::SHF_ALLOC | elf::SHF_WRITE,     16,   0},
				{".eh_frame",       elf::SHT_X86_64_UNWIND, elf::SHF_ALLOC,                      8,    0},
				{".debug_abbrev",   elf::SHT_PROGBITS,      0,                                   1,    0},
				{".debug_info",     elf::SHT_PROGBITS,      0,                                   1,    0},
				{".debug_line",     elf::SHT_PROGBITS,      0,                                   1,    0},
				{".debug_rnglists", elf::SHT_PROGBITS,      0,                                   1,    0},
				{".debug_loclists", elf::SHT_PROGBITS,      0,                                   1,    0},
		};
		// clang-format on
		static_assert(sizeof(kElfSections) / sizeof(kElfSections[0]) == ObjectFile::SectionCount,
									"one entry per Section");

		struct ElfSection {
			elf::SectionHeader header;
			const List<U8>* bytes; // null = no file data
		};
		void writeElfImage(std::ostream& os, List<ElfSection>& secs, U16 shstrIndex);
		void coffName(List<U8>& out, List<U8>& strtab, const String& n);
	} // namespace detail
} // namespace rat

#endif
