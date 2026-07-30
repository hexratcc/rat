#ifndef RAT_RATL_ELFREAD_H
#define RAT_RATL_ELFREAD_H

#include "Core.h"
#include "ElfFile.h"

namespace rat {
	enum Bucket {
		BText,		// ALLOC|EXECINSTR
		BRodata,	// ALLOC ro
		BEhFrame, // own section so .eh_frame_hdr can index it
		BInitArray,
		BFiniArray,
		BData,	// ALLOC|WRITE progbits
		BBss,		// ALLOC|WRITE nobits
		BTdata, // tls progbits
		BTbss,	// tls nobits
		BucketCount,
		BNone = 0xff
	};

	struct InSection {
		String name;
		U32 type = 0;
		U64 flags = 0;
		U64 align = 1;
		U64 fileOff = 0; // offset in image
		U64 size = 0;
		U8 bucket = BNone;
		U64 outOff = 0; // offset in bucket, filled at layout
		B32 keep = false;
	};

	struct InSym {
		String name;
		U32 shndx = 0; // section index or SHN_*
		U64 value = 0;
		U64 size = 0;
		U8 type = elf::STT_NOTYPE;
		U8 bind = elf::STB_LOCAL;
		U8 other = elf::STV_DEFAULT;
		B32 undef = true;
		B32 common = false;
		B32 abs = false;
	};

	struct InRel {
		U32 secIdx = 0; // patched section
		U64 offset = 0;
		U32 sym = 0;
		U32 type = 0;
		I64 addend = 0;
	};

	struct InObject {
		String path;
		List<U8> image; // whole file, keeps section bytes valid
		List<InSection> sections;
		List<InSym> syms;
		List<InRel> rels;
	};

	struct ArchiveFile {
		String path;
		List<U8> data;
		Map<String, U64> index; // symbol -> member header offset
		Set<U64> pulled;
	};

	// shared lib, resident only for its dynsym
	struct Lib {
		String soname;
		List<U8> dynsym;
		List<U8> dynstr;
	};

	// external symbol from a shared lib
	struct Import {
		enum Kind { Func, Data } kind;
		U32 dynIndex = 0; // index in .dynsym
		U64 addr = 0;			// PLT stub (Func) or .bss COPY (Data)
		U64 size = 0;
		U32 pltIndex = 0;
	};

	// GOT slot for a GOTPCREL-family reloc
	struct GotSlot {
		B32 isImport = false; // needs GLOB_DAT
		U32 dynIndex = 0;			// imports
		U64 addr = 0;					// defined targets, filled at write
		B32 defined = false;
	};

	B32 readWhole(const String& path, List<U8>& out);

	B32 loadObject(List<U8> img, const String& path, InObject& obj, String& err);

	U64 arMemberSize(const List<U8>& d, U64 hdrOff);
	B32 parseArchive(const String& path, List<U8> bytes, ArchiveFile& ar, String& err);
	B32 loadLibrary(const String& path, Lib& lib, String& err);
	B32 findLibrary(const String& l, const List<String>& paths, String& found);

	// host loader + lib dirs probed from ratl's own image, used when opt empty
	const String& hostLoader();
	const List<String>& hostLibDirs();
} // namespace rat

#endif
