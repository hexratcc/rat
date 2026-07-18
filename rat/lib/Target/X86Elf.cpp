#include "Target/X86Elf.h"

namespace rat {
	namespace detail {
		constexpr U8 kElfMag[4] = {0x7f, 'E', 'L', 'F'};
		constexpr U8 ELFCLASS64 = 2;
		constexpr U8 ELFDATA2LSB = 1;
		constexpr U8 EV_CURRENT = 1;
		constexpr U16 ET_REL = 1;
		constexpr U16 EM_X86_64 = 62;

		constexpr U32 SHT_NULL = 0;
		constexpr U32 SHT_PROGBITS = 1;
		constexpr U32 SHT_SYMTAB = 2;
		constexpr U32 SHT_STRTAB = 3;
		constexpr U32 SHT_RELA = 4;
		constexpr U32 SHT_NOBITS = 8;

		constexpr U64 SHF_WRITE = 0x1;
		constexpr U64 SHF_ALLOC = 0x2;
		constexpr U64 SHF_EXECINSTR = 0x4;
		constexpr U64 SHF_INFO_LINK = 0x40;

		constexpr U8 STB_LOCAL = 0;
		constexpr U8 STB_GLOBAL = 1;
		constexpr U8 STT_NOTYPE = 0;
		constexpr U8 STT_OBJECT = 1;
		constexpr U8 STT_FUNC = 2;
		constexpr U16 SHN_UNDEF = 0;

		constexpr U64 kEhSize = 64;			 // Elf64_Ehdr
		constexpr U64 kShEntSize = 64;	 // Elf64_Shdr
		constexpr U64 kSymEntSize = 24;	 // Elf64_Sym
		constexpr U64 kRelaEntSize = 24; // Elf64_Rela
	} // namespace detail

	void ElfObject::write(std::ostream& os) {
		List<U32> order;
		order.push_back(0);
		for(U32 i = 1; i < syms.size(); ++i)
			if(!syms[i].global)
				order.push_back(i);
		U32 firstGlobal = (U32)order.size();
		for(U32 i = 1; i < syms.size(); ++i)
			if(syms[i].global)
				order.push_back(i);

		List<U32> remap(syms.size(), 0);
		for(U32 i = 0; i < order.size(); ++i)
			remap[order[i]] = i;

		List<U8> strtab;
		strtab.push_back(0);
		List<U32> nameOff(syms.size(), 0);
		for(U32 i = 1; i < syms.size(); ++i) {
			nameOff[i] = (U32)strtab.size();
			const String& n = syms[i].name;
			strtab.insert(strtab.end(), n.begin(), n.end());
			strtab.push_back(0);
		}

		constexpr U32 shText = 1, shRodata = 3, shData = 5, shBss = 7, shSymtab = 8, shStrtab = 9,
									shShstrtab = 10;
		static constexpr U32 kSecShIndex[] = {shText, shRodata, shData, shBss};
		auto secShIndex = [](Section s) -> U32 { return kSecShIndex[(U32)s]; };

		List<U8> symtab;
		for(U32 i = 0; i < order.size(); ++i) {
			U32 oi = order[i];
			const Sym& s = syms[oi];
			B32 placed = i != 0 && s.defined;
			U8 bind = (i != 0 && s.global) ? detail::STB_GLOBAL : detail::STB_LOCAL;
			U8 type = !placed ? detail::STT_NOTYPE : (s.isFunc ? detail::STT_FUNC : detail::STT_OBJECT);
			U16 shndx = !placed ? detail::SHN_UNDEF : (U16)secShIndex(s.sec);

			put32(symtab, i == 0 ? 0u : nameOff[oi]);				// st_name
			put8(symtab, (U8)((bind << 4) | (type & 0xf))); // st_info
			put8(symtab, 0);																// st_other
			put16(symtab, shndx);														// st_shndx
			put64(symtab, placed ? s.offset : 0u);					// st_value
			put64(symtab, 0);																// st_size
		}

		auto buildRela = [&](Section target) {
			List<U8> out;
			for(const Rel& r : relocs) {
				if(r.sec != target)
					continue;
				U64 info = ((U64)remap[r.symIndex] << 32) | (U64)(U32)r.kind;
				put64(out, r.offset);			 // r_offset
				put64(out, info);					 // r_info
				put64(out, (U64)r.addend); // r_addend
			}
			return out;
		};
		List<U8> relaText = buildRela(Text);
		List<U8> relaRodata = buildRela(Rodata);
		List<U8> relaData = buildRela(Data);

		List<U8> shstr;
		shstr.push_back(0);
		auto addShName = [&](const C8* n) {
			U32 off = (U32)shstr.size();
			for(const C8* p = n; *p; ++p)
				shstr.push_back((U8)*p);
			shstr.push_back(0);
			return off;
		};
		U32 nText = addShName(".text");
		U32 nRelaText = addShName(".rela.text");
		U32 nRodata = addShName(".rodata");
		U32 nRelaRodata = addShName(".rela.rodata");
		U32 nData = addShName(".data");
		U32 nRelaData = addShName(".rela.data");
		U32 nBss = addShName(".bss");
		U32 nSymtab = addShName(".symtab");
		U32 nStrtab = addShName(".strtab");
		U32 nShstrtab = addShName(".shstrtab");
		U32 nNoteStack = addShName(".note.GNU-stack");

		const U32 shCount = 12;
		U64 off = detail::kEhSize;
		auto place = [&](U64 size, U64 a) {
			off = (off + (a - 1)) & ~(a - 1);
			U64 here = off;
			off += size;
			return here;
		};
		U64 offText = place(bytesOf(Text).size(), 16);
		U64 offRodata = place(bytesOf(Rodata).size(), 16);
		U64 offData = place(bytesOf(Data).size(), 16);
		U64 offSymtab = place(symtab.size(), 8);
		U64 offStrtab = place(strtab.size(), 1);
		U64 offRelaText = place(relaText.size(), 8);
		U64 offRelaRodata = place(relaRodata.size(), 8);
		U64 offRelaData = place(relaData.size(), 8);
		U64 offShstr = place(shstr.size(), 1);
		U64 offSh = (off + 7) & ~7ull; // section header table

		List<U8> out;
		for(U8 c : detail::kElfMag)
			put8(out, c);
		put8(out, detail::ELFCLASS64);
		put8(out, detail::ELFDATA2LSB);
		put8(out, detail::EV_CURRENT);
		put8(out, 0); // System V
		for(U32 i = 0; i < 8; ++i)
			put8(out, 0); // ABIVERSION + padding
		put16(out, detail::ET_REL);
		put16(out, detail::EM_X86_64);
		put32(out, detail::EV_CURRENT);
		put64(out, 0);											 // e_entry
		put64(out, 0);											 // e_phoff
		put64(out, offSh);									 // e_shoff
		put32(out, 0);											 // e_flags
		put16(out, (U16)detail::kEhSize);		 // e_ehsize
		put16(out, 0);											 // e_phentsize
		put16(out, 0);											 // e_phnum
		put16(out, (U16)detail::kShEntSize); // e_shentsize
		put16(out, (U16)shCount);						 // e_shnum
		put16(out, (U16)shShstrtab);				 // e_shstrndx

		auto emitAt = [&](U64 target, const List<U8>& blob) {
			padTo(out, target);
			out.insert(out.end(), blob.begin(), blob.end());
		};
		emitAt(offText, bytesOf(Text));
		emitAt(offRodata, bytesOf(Rodata));
		emitAt(offData, bytesOf(Data));
		emitAt(offSymtab, symtab);
		emitAt(offStrtab, strtab);
		emitAt(offRelaText, relaText);
		emitAt(offRelaRodata, relaRodata);
		emitAt(offRelaData, relaData);
		emitAt(offShstr, shstr);
		padTo(out, offSh);

		struct ShDesc {
			U32 name;
			U32 type;
			U64 flags;
			U64 fileOff;
			U64 size;
			U32 link;
			U32 info;
			U64 align;
			U64 entSize;
		};
		const U64 alloc = detail::SHF_ALLOC;
		const U64 rela = detail::SHF_INFO_LINK;
		const ShDesc headers[] = {
				// clang-format off
				// name        type                  flags                          offset         size               link      info         align entsize
				{0,           detail::SHT_NULL,     0,                             0,             0,                 0,        0,           0,    0},                    // 0 null
				{nText,       detail::SHT_PROGBITS, alloc | detail::SHF_EXECINSTR, offText,       bytesOf(Text).size(),        0,           0,    16,   0},              // 1 .text
				{nRelaText,   detail::SHT_RELA,     rela,                          offRelaText,   relaText.size(),   shSymtab, shText,      8,    detail::kRelaEntSize}, // 2 .rela.text
				{nRodata,     detail::SHT_PROGBITS, alloc,                         offRodata,     bytesOf(Rodata).size(),      0,           0,    16,   0},              // 3 .rodata
				{nRelaRodata, detail::SHT_RELA,     rela,                          offRelaRodata, relaRodata.size(), shSymtab, shRodata,    8,    detail::kRelaEntSize}, // 4 .rela.rodata
				{nData,       detail::SHT_PROGBITS, alloc | detail::SHF_WRITE,     offData,       bytesOf(Data).size(),        0,           0,    16,   0},              // 5 .data
				{nRelaData,   detail::SHT_RELA,     rela,                          offRelaData,   relaData.size(),   shSymtab, shData,      8,    detail::kRelaEntSize}, // 6 .rela.data
				{nBss,        detail::SHT_NOBITS,   alloc | detail::SHF_WRITE,     0,             bssSize,           0,        0,           16,   0},                    // 7 .bss
				{nSymtab,     detail::SHT_SYMTAB,   0,                             offSymtab,     symtab.size(),     shStrtab, firstGlobal, 8,    detail::kSymEntSize},  // 8 .symtab
				{nStrtab,     detail::SHT_STRTAB,   0,                             offStrtab,     strtab.size(),     0,        0,           1,    0},                    // 9 .strtab
				{nShstrtab,   detail::SHT_STRTAB,   0,                             offShstr,      shstr.size(),      0,        0,           1,    0},                    // 10 .shstrtab
				{nNoteStack,  detail::SHT_PROGBITS, 0,                             0,             0,                 0,        0,           1,    0},                    // 11 .note.GNU-stack
				// clang-format on
		};
		static_assert(sizeof(headers) / sizeof(headers[0]) == shCount, "headers must match shCount");
		for(const ShDesc& h : headers) {
			put32(out, h.name);
			put32(out, h.type);
			put64(out, h.flags);
			put64(out, 0); // sh_addr: always 0 in a relocatable object
			put64(out, h.fileOff);
			put64(out, h.size);
			put32(out, h.link);
			put32(out, h.info);
			put64(out, h.align);
			put64(out, h.entSize);
		}

		os.write(reinterpret_cast<const C8*>(out.data()), (std::streamsize)out.size());
	}
} // namespace rat
