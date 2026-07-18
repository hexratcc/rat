#include "Target/X86Coff.h"

#include <cstring>

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
		constexpr U32 kSymEntSize = 18;
		constexpr U32 kRelocEntSize = 10;
	} // namespace detail

	void CoffObject::write(std::ostream& os) {
		constexpr U32 kNumSections = 4;
		const C8* secNames[kNumSections] = {".text", ".rdata", ".data", ".bss"};
		const U32 secFlags[kNumSections] = {
				detail::IMAGE_SCN_CNT_CODE | detail::IMAGE_SCN_MEM_EXECUTE | detail::IMAGE_SCN_MEM_READ |
						detail::IMAGE_SCN_ALIGN_16BYTES,
				detail::IMAGE_SCN_CNT_INITIALIZED_DATA | detail::IMAGE_SCN_MEM_READ |
						detail::IMAGE_SCN_ALIGN_16BYTES,
				detail::IMAGE_SCN_CNT_INITIALIZED_DATA | detail::IMAGE_SCN_MEM_READ |
						detail::IMAGE_SCN_MEM_WRITE | detail::IMAGE_SCN_ALIGN_16BYTES,
				detail::IMAGE_SCN_CNT_UNINITIALIZED_DATA | detail::IMAGE_SCN_MEM_READ |
						detail::IMAGE_SCN_MEM_WRITE | detail::IMAGE_SCN_ALIGN_8BYTES,
		};

		List<U8> secBytes[kByteSections] = {bytesOf(Text), bytesOf(Rodata), bytesOf(Data)};
		auto patchField = [&](Section sec, U32 offset, U64 v, U32 len) {
			List<U8>& b = secBytes[(U32)sec];
			for(U32 i = 0; i < len; ++i)
				b[offset + i] = (U8)(v >> (i * 8));
		};
		for(const Rel& r : relocs) {
			if(r.sec == Bss)
				continue;
			if(r.kind == RelocKind::Abs64)
				patchField(r.sec, r.offset, (U64)r.addend, 8);
			else
				patchField(r.sec, r.offset, (U64)(U32)(I32)(r.addend + 4), 4);
		}

		// symbol table
		List<U8> strtab;
		put32(strtab, 0); // patched to the final size below
		auto nameField = [&](List<U8>& out, const String& n) {
			if(n.size() <= 8) {
				for(U32 i = 0; i < 8; ++i)
					put8(out, i < n.size() ? (U8)n[i] : 0);
				return;
			}
			U32 off = (U32)strtab.size();
			strtab.insert(strtab.end(), n.begin(), n.end());
			strtab.push_back(0);
			put32(out, 0);
			put32(out, off);
		};

		U32 relocCount[kNumSections] = {};
		for(const Rel& r : relocs)
			++relocCount[(U32)r.sec];

		List<U8> symtab;
		U32 symEntries = 0;
		for(U32 s = 0; s < kNumSections; ++s) {
			nameField(symtab, secNames[s]);
			put32(symtab, 0);						 // value
			put16(symtab, (U16)(s + 1)); // section number (1-based)
			put16(symtab, 0);						 // type
			put8(symtab, detail::IMAGE_SYM_CLASS_STATIC);
			put8(symtab, 1); // one aux record
			// aux: section definition
			put32(symtab, sectionSize((Section)s)); // length
			put16(symtab, (U16)relocCount[s]);			// number of relocations
			put16(symtab, 0);												// number of line numbers
			put32(symtab, 0);												// checksum
			put16(symtab, 0);												// associated section
			put8(symtab, 0);												// selection
			put8(symtab, 0);												// padding to 18 bytes
			put16(symtab, 0);
			symEntries += 2;
		}

		List<U32> symCoffIndex(syms.size(), 0);
		for(U32 i = 1; i < syms.size(); ++i) {
			const Sym& s = syms[i];
			symCoffIndex[i] = symEntries;
			nameField(symtab, s.name);
			put32(symtab, s.defined ? s.offset : 0);												 // value
			put16(symtab, s.defined ? (U16)((U32)s.sec + 1) : 0);						 // section (0 = undefined)
			put16(symtab, s.defined && s.isFunc ? detail::kSymTypeFunc : 0); // type
			// undefined symbols must be EXTERNAL for linker to res
			put8(symtab,
					 !s.defined || s.global ? detail::IMAGE_SYM_CLASS_EXTERNAL
																	: detail::IMAGE_SYM_CLASS_STATIC);
			put8(symtab, 0); // no aux records
			symEntries += 1;
		}

		// patch the string table size prefix
		for(U32 i = 0; i < 4; ++i)
			strtab[i] = (U8)((U32)strtab.size() >> (i * 8));

		U32 off = detail::kFileHeaderSize + kNumSections * detail::kSectionHeaderSize;
		U32 rawOff[kNumSections] = {};
		for(U32 s = 0; s + 1 < kNumSections; ++s) { // .bss has no raw data
			off = (off + 15u) & ~15u;
			rawOff[s] = secBytes[s].empty() ? 0 : off;
			off += (U32)secBytes[s].size();
		}
		U32 relOff[kNumSections] = {};
		for(U32 s = 0; s < kNumSections; ++s) {
			relOff[s] = relocCount[s] ? off : 0;
			off += relocCount[s] * detail::kRelocEntSize;
		}
		U32 symOff = off;

		List<U8> out;
		put16(out, detail::IMAGE_FILE_MACHINE_AMD64);
		put16(out, (U16)kNumSections);
		put32(out, 0); // timestamp
		put32(out, symOff);
		put32(out, symEntries);
		put16(out, 0); // optional header size
		put16(out, 0); // characteristics

		for(U32 s = 0; s < kNumSections; ++s) {
			U64 nameLen = std::strlen(secNames[s]);
			for(U32 i = 0; i < 8; ++i)
				put8(out, i < nameLen ? (U8)secNames[s][i] : 0);
			put32(out, 0); // virtual size
			put32(out, 0); // virtual address
			put32(out, sectionSize((Section)s));
			put32(out, s + 1 < kNumSections ? rawOff[s] : 0);
			put32(out, relOff[s]);
			put32(out, 0); // line numbers
			put16(out, (U16)relocCount[s]);
			put16(out, 0); // line number count
			put32(out, secFlags[s]);
		}

		for(U32 s = 0; s + 1 < kNumSections; ++s) {
			if(secBytes[s].empty())
				continue;
			padTo(out, rawOff[s]);
			out.insert(out.end(), secBytes[s].begin(), secBytes[s].end());
		}

		// pad out to the first relocation block
		U32 firstTail = symOff;
		for(U32 s = 0; s < kNumSections; ++s)
			if(relOff[s]) {
				firstTail = relOff[s];
				break;
			}
		padTo(out, firstTail);
		List<const Rel*> relBySec[kNumSections];
		for(const Rel& r : relocs)
			relBySec[(U32)r.sec].push_back(&r);
		for(U32 s = 0; s < kNumSections; ++s) {
			for(const Rel* r : relBySec[s]) {
				put32(out, r->offset);
				put32(out, symCoffIndex[r->symIndex]);
				put16(out,
							r->kind == RelocKind::Abs64 ? detail::IMAGE_REL_AMD64_ADDR64
																					: detail::IMAGE_REL_AMD64_REL32);
			}
		}

		out.insert(out.end(), symtab.begin(), symtab.end());
		out.insert(out.end(), strtab.begin(), strtab.end());

		os.write(reinterpret_cast<const C8*>(out.data()), (std::streamsize)out.size());
	}
} // namespace rat
