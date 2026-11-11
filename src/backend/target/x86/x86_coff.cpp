#include "target/object_file.h"

#include "byte_io.h"

#include <ostream>

namespace rat {
	namespace detail {
		constexpr U16 IMAGE_FILE_MACHINE_AMD64 = 0x8664;

		constexpr U32 IMAGE_SCN_CNT_CODE = 0x00000020;
		constexpr U32 IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
		constexpr U32 IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
		constexpr U32 IMAGE_SCN_ALIGN_8BYTES = 0x00400000;
		constexpr U32 IMAGE_SCN_ALIGN_16BYTES = 0x00500000;
		constexpr U32 IMAGE_SCN_MEM_EXECUTE = 0x20000000;
		constexpr U32 IMAGE_SCN_MEM_READ = 0x40000000;
		constexpr U32 IMAGE_SCN_MEM_WRITE = 0x80000000;

		constexpr U16 IMAGE_REL_AMD64_ADDR64 = 0x0001;
		constexpr U16 IMAGE_REL_AMD64_REL32 = 0x0004;

		constexpr U8 IMAGE_SYM_CLASS_EXTERNAL = 2;
		constexpr U8 IMAGE_SYM_CLASS_STATIC = 3;

		constexpr U16 kSymTypeFunc = 0x20; // DTYPE_FUNCTION << 4

		constexpr U32 kFileHeaderSize = 20;
		constexpr U32 kSectionHeaderSize = 40;
		constexpr U32 kRelocEntSize = 10;
		constexpr U32 kSymEntSize = 18;

		constexpr U32 kCoffSections = ObjectFile::Bss + 1; // Text..Bss
		constexpr U32 kCoffByteSections = ObjectFile::Bss; // the ones with raw data

		struct CoffSection {
			const C8* name;
			U32 flags;
		};
		constexpr U32 kRead = IMAGE_SCN_MEM_READ;
		constexpr U32 kReadWrite = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
		constexpr CoffSection kCoffSecs[kCoffSections] = {
				{".text", IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | kRead | IMAGE_SCN_ALIGN_16BYTES},
				{".rdata", IMAGE_SCN_CNT_INITIALIZED_DATA | kRead | IMAGE_SCN_ALIGN_16BYTES},
				{".data", IMAGE_SCN_CNT_INITIALIZED_DATA | kReadWrite | IMAGE_SCN_ALIGN_16BYTES},
				{".bss", IMAGE_SCN_CNT_UNINITIALIZED_DATA | kReadWrite | IMAGE_SCN_ALIGN_8BYTES},
		};

		void coffName(List<U8>& out, List<U8>& strtab, const String& n) {
			if(n.size() <= 8) {
				out.insert(out.end(), n.begin(), n.end());
				out.insert(out.end(), 8 - n.size(), 0);
				return;
			}
			le::put32(out, 0);
			le::put32(out, elf::appendName(strtab, n));
		}
	} // namespace detail

	List<U8> ObjectFile::coffSymtab(const RelBuckets rels, List<U8>& strtab, List<U32>& index) const {
		List<U8> symtab;
		for(U32 s = 0; s < detail::kCoffSections; ++s) {
			detail::coffName(symtab, strtab, detail::kCoffSecs[s].name);
			le::put32(symtab, 0);						 // value
			le::put16(symtab, (U16)(s + 1)); // section number (1-based)
			le::put16(symtab, 0);						 // type
			le::put8(symtab, detail::IMAGE_SYM_CLASS_STATIC);
			le::put8(symtab, 1); // one aux record
			// aux: section definition
			le::put32(symtab, sectionSize((Section)s)); // length
			le::put16(symtab, (U16)rels[s].size());			// number of relocations
			le::padTo(symtab,
								symtab.size() + 12); // line numbers, checksum, associated section, selection
		}

		index.assign(syms.size(), 0);
		for(U32 i = 1; i < syms.size(); ++i) {
			const Sym& s = syms[i];
			index[i] = (U32)(symtab.size() / detail::kSymEntSize);
			detail::coffName(symtab, strtab, s.name);
			le::put32(symtab, s.defined ? s.offset : 0);							// value
			le::put16(symtab, s.defined ? (U16)((U32)s.sec + 1) : 0); // section (0 = undefined)
			le::put16(symtab, s.defined && s.isFunc ? detail::kSymTypeFunc : 0); // type
			// undefined symbols must be EXTERNAL for linker to res
			le::put8(symtab,
							 !s.defined || s.global ? detail::IMAGE_SYM_CLASS_EXTERNAL
																			: detail::IMAGE_SYM_CLASS_STATIC);
			le::put8(symtab, 0); // no aux records
		}
		return symtab;
	}

	// headers, raw data 16 aligned, relocations, symbols, strings. coff has no rela, the addend goes
	// into the patched bytes
	void ObjectFile::writeCoff(std::ostream& os) {
		RelBuckets relBySec;
		partitionRelocs(relBySec);
		for(U32 s = 0; s < detail::kCoffByteSections; ++s)
			for(const Rel* r : relBySec[s]) {
				U8* patch = &bytesOf(r->sec)[r->offset];
				if(r->kind == RelocKind::Abs64)
					le::wr(patch, (U64)r->addend, 8);
				else
					le::wr(patch, (U32)(I32)(r->addend + 4), 4);
			}
		List<U8> strtab(4, 0); // its size, patched last
		List<U32> symCoffIndex;
		List<U8> symtab = coffSymtab(relBySec, strtab, symCoffIndex);

		U32 off = detail::kFileHeaderSize + detail::kCoffSections * detail::kSectionHeaderSize;
		U32 rawOff[detail::kCoffSections] = {}, relOff[detail::kCoffSections] = {};
		for(U32 s = 0; s < detail::kCoffByteSections; ++s) {
			off = (U32)alignUp(off, 16);
			rawOff[s] = raw[s].empty() ? 0 : off;
			off += (U32)raw[s].size();
		}
		U32 relStart = off;
		for(U32 s = 0; s < detail::kCoffSections; ++s) {
			relOff[s] = relBySec[s].empty() ? 0 : off;
			off += (U32)relBySec[s].size() * detail::kRelocEntSize;
		}

		List<U8> out;
		le::put16(out, detail::IMAGE_FILE_MACHINE_AMD64);
		le::put16(out, (U16)detail::kCoffSections);
		le::put32(out, 0); // timestamp
		le::put32(out, off);
		le::put32(out, (U32)(symtab.size() / detail::kSymEntSize));
		le::put32(out, 0); // optional header size, characteristics
		for(U32 s = 0; s < detail::kCoffSections; ++s) {
			detail::coffName(out, strtab, detail::kCoffSecs[s].name);
			le::put64(out, 0); // virtual size and address
			le::put32(out, sectionSize((Section)s));
			le::put32(out, rawOff[s]);
			le::put32(out, relOff[s]);
			le::put32(out, 0); // line numbers
			le::put16(out, (U16)relBySec[s].size());
			le::put16(out, 0); // line number count
			le::put32(out, detail::kCoffSecs[s].flags);
		}
		for(U32 s = 0; s < detail::kCoffByteSections; ++s) {
			le::padTo(out, rawOff[s]);
			out.insert(out.end(), raw[s].begin(), raw[s].end());
		}
		le::padTo(out, relStart);
		for(U32 s = 0; s < detail::kCoffSections; ++s)
			for(const Rel* r : relBySec[s]) {
				le::put32(out, r->offset);
				le::put32(out, symCoffIndex[r->symIndex]);
				B32 isAbs64 = r->kind == RelocKind::Abs64;
				le::put16(out, isAbs64 ? detail::IMAGE_REL_AMD64_ADDR64 : detail::IMAGE_REL_AMD64_REL32);
			}
		out.insert(out.end(), symtab.begin(), symtab.end());
		le::patch32(strtab, 0, (U32)strtab.size());
		out.insert(out.end(), strtab.begin(), strtab.end());
		os.write(reinterpret_cast<const C8*>(out.data()), (std::streamsize)out.size());
	}
} // namespace rat
