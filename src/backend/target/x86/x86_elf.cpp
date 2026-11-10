#include "target/object_file.h"

#include <algorithm>
#include <ostream>

namespace rat {
	namespace detail {
		constexpr U64 kSymEntSize = 24;	 // Elf64_Sym
		constexpr U64 kRelaEntSize = 24; // Elf64_Rela

		constexpr elf::SectionInfo kSymtab = {".symtab", elf::SHT_SYMTAB, 0, 8, kSymEntSize};
		constexpr elf::SectionInfo kStrtab = {".strtab", elf::SHT_STRTAB, 0, 1, 0};
		constexpr elf::SectionInfo kShstrtab = {".shstrtab", elf::SHT_STRTAB, 0, 1, 0};
		constexpr elf::SectionInfo kNoteStack = {".note.GNU-stack", elf::SHT_PROGBITS, 0, 1, 0};
	} // namespace detail

	U32 ObjectFile::elfSymtab(List<U8>& symtab,
														List<U8>& strtab,
														List<U32>& remap,
														const List<U32>& shIndex) const {
		List<U32> order;
		order.push_back(0);
		for(U32 i = 1; i < syms.size(); ++i)
			if(!syms[i].global)
				order.push_back(i);
		U32 firstGlobal = (U32)order.size();
		for(U32 i = 1; i < syms.size(); ++i)
			if(syms[i].global)
				order.push_back(i);

		remap.assign(syms.size(), 0);
		for(U32 i = 0; i < order.size(); ++i)
			remap[order[i]] = i;

		strtab.push_back(0);
		List<U32> nameOff(syms.size(), 0);
		for(U32 i = 1; i < syms.size(); ++i)
			nameOff[i] = elf::appendName(strtab, syms[i].name);

		for(U32 i = 0; i < order.size(); ++i) {
			U32 oi = order[i];
			const Sym& s = syms[oi];
			B32 placed = i != 0 && s.defined;
			U8 bind = (i != 0 && s.global) ? elf::STB_GLOBAL : elf::STB_LOCAL;
			U8 type = !placed ? elf::STT_NOTYPE : (s.isFunc ? elf::STT_FUNC : elf::STT_OBJECT);
			if(placed && s.isSection)
				type = elf::STT_SECTION;
			U16 shndx = !placed ? elf::SHN_UNDEF : (U16)shIndex[s.sec];

			le::put32(symtab, i == 0 ? 0u : nameOff[oi]);				// st_name
			le::put8(symtab, (U8)((bind << 4) | (type & 0xf))); // st_info
			le::put8(symtab, 0);																// st_other
			le::put16(symtab, shndx);														// st_shndx
			le::put64(symtab, placed ? s.offset : 0u);					// st_value
			le::put64(symtab, 0);																// st_size
		}
		return firstGlobal;
	}

	List<U8> ObjectFile::elfRela(const List<const Rel*>& bucket, const List<U32>& remap) {
		List<U8> out;
		out.reserve(bucket.size() * detail::kRelaEntSize);
		for(const Rel* r : bucket) {
			U64 info = ((U64)remap[r->symIndex] << 32) | (U64)(U32)r->kind;
			le::put64(out, r->offset);			// r_offset
			le::put64(out, info);						// r_info
			le::put64(out, (U64)r->addend); // r_addend
		}
		return out;
	}

	void ObjectFile::writeElf(std::ostream& os) {
		RelBuckets relBySec;
		partitionRelocs(relBySec);

		// the classic sections always, the others only when used, each followed by its rela
		List<elf::OutSection> secs(1);
		List<U8> shstr(1, 0);
		List<U32> shIndex(SectionCount, 0), relaIndex(SectionCount, 0);
		for(U32 s = 0; s < SectionCount; ++s) {
			Section sec = (Section)s;
			const elf::SectionInfo& info = detail::kElfSections[s];
			B32 optional = sec >= EhFrame;
			if(optional && raw[s].empty())
				continue;
			shIndex[s] = (U32)secs.size();
			elf::SectionHeader header = elf::headerOf(info, shstr);
			header.size = sectionSize(sec);
			header.align = std::max<U64>(info.align, requestedAlign[s]);
			secs.push_back({header, sec == Bss ? nullptr : &raw[s]});
			if(sec == Bss || (optional && relBySec[s].empty()))
				continue;
			relaIndex[s] = (U32)secs.size();
			String relaName = String(".rela") + info.name;
			elf::SectionHeader rela = elf::headerOf(
					{relaName.c_str(), elf::SHT_RELA, elf::SHF_INFO_LINK, 8, detail::kRelaEntSize}, shstr);
			rela.info = shIndex[s];
			secs.push_back({rela, nullptr});
		}

		U32 shSymtab = (U32)secs.size();
		List<U8> symtab, strtab;
		List<U32> remap;
		U32 firstGlobal = elfSymtab(symtab, strtab, remap, shIndex);
		List<U8> rela[SectionCount];
		for(U32 s = 0; s < SectionCount; ++s) {
			if(!relaIndex[s])
				continue;
			rela[s] = elfRela(relBySec[s], remap);
			secs[relaIndex[s]].bytes = &rela[s];
			secs[relaIndex[s]].header.link = shSymtab;
		}

		elf::SectionHeader symtabHeader = elf::headerOf(detail::kSymtab, shstr);
		symtabHeader.link = shSymtab + 1;
		symtabHeader.info = firstGlobal;
		secs.push_back({symtabHeader, &symtab});
		secs.push_back({elf::headerOf(detail::kStrtab, shstr), &strtab});
		U16 shShstrtab = (U16)secs.size();
		secs.push_back({elf::headerOf(detail::kShstrtab, shstr), &shstr});
		secs.push_back({elf::headerOf(detail::kNoteStack, shstr), nullptr});
		List<U8> out;
		elf::putFileHeader(out, elf::ET_REL, 0, 0);
		elf::appendSections(out, secs, shShstrtab);
		os.write(reinterpret_cast<const C8*>(out.data()), (std::streamsize)out.size());
	}
} // namespace rat
