#include "linker_internal.h"

#include "elf_file.h"

#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace rat {
	using namespace elf;

	namespace detail {
		constexpr U64 kImageBase = 0x400000;
		constexpr U64 kPage = 0x1000;
		constexpr U64 kPltEntSize = 16;

		// the alloc sections gdb, objdump and readelf look at, in address order
		struct OutSectionInfo {
			Linker::OutSec sec;
			SectionInfo info;
		};
		// clang-format off
		constexpr OutSectionInfo kShdrSections[] = {
				// section            name           type            flags                            align entsize
				{Linker::ODynstr,    {".dynstr",     SHT_STRTAB,     SHF_ALLOC,                       1,  0}},
				{Linker::OText,      {".text",       SHT_PROGBITS,   SHF_ALLOC | SHF_EXECINSTR,       16, 0}},
				{Linker::ORodata,    {".rodata",     SHT_PROGBITS,   SHF_ALLOC,                       16, 0}},
				{Linker::OEhFrame,   {".eh_frame",   SHT_PROGBITS,   SHF_ALLOC,                       8,  0}},
				{Linker::OInitArray, {".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE,           8,  8}},
				{Linker::OFiniArray, {".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE,           8,  8}},
				{Linker::OData,      {".data",       SHT_PROGBITS,   SHF_ALLOC | SHF_WRITE,           16, 0}},
				{Linker::OTdata,     {".tdata",      SHT_PROGBITS,   SHF_ALLOC | SHF_WRITE | SHF_TLS, 16, 0}},
				{Linker::ODynamic,   {".dynamic",    SHT_DYNAMIC,    SHF_ALLOC | SHF_WRITE,           8,  16}},
				{Linker::OBss,       {".bss",        SHT_NOBITS,     SHF_ALLOC | SHF_WRITE,           16, 0}},
				{Linker::OTbss,      {".tbss",       SHT_NOBITS,     SHF_ALLOC | SHF_WRITE | SHF_TLS, 16, 0}},
		};
		// clang-format on

		constexpr U16 kPhnum = 8; // PHDR INTERP LOAD LOAD DYNAMIC TLS GNU_EH_FRAME GNU_STACK

		// clang-format off
		// _start calls __libc_start_main(main, argc, argv, 0, 0, rtld_fini, stack_end)
		constexpr U8 kStartCode[] = {
				0x31, 0xed,             // xor  ebp, ebp
				0x49, 0x89, 0xd1,       // mov  r9, rdx
				0x5e,                   // pop  rsi
				0x48, 0x89, 0xe2,       // mov  rdx, rsp
				0x48, 0x83, 0xe4, 0xf0, // and  rsp, -16
				0x50,                   // push rax
				0x54,                   // push rsp
				0x45, 0x31, 0xc0,       // xor  r8d, r8d
				0x31, 0xc9,             // xor  ecx, ecx
				0x48, 0x8d, 0x3d,       // lea  rdi, [rip+disp32]
				0, 0, 0, 0,
				0xe8,                   // call rel32
				0, 0, 0, 0,
				0xf4,                   // hlt
		};
		// clang-format on
		// the disp32 fields of the lea and the call
		constexpr U64 kStartMainDisp = 23;
		constexpr U64 kStartCallDisp = 28;

		// per Bucket
		constexpr Linker::OutSec kBucketSecs[BucketCount] = {Linker::OText,
																												 Linker::ORodata,
																												 Linker::OEhFrame,
																												 Linker::OInitArray,
																												 Linker::OFiniArray,
																												 Linker::OData,
																												 Linker::OBss,
																												 Linker::OTdata,
																												 Linker::OTbss};

		struct LinkerSym {
			const C8* name;
			Linker::OutSec sec; // kOutSecs is the file header
			B32 atEnd;
		};
		constexpr LinkerSym kLinkerSyms[] = {
				{"__dso_handle", Linker::OData, false},
				{"_GLOBAL_OFFSET_TABLE_", Linker::OGotPlt, false},
				{"__init_array_start", Linker::OInitArray, false},
				{"__init_array_end", Linker::OInitArray, true},
				{"__preinit_array_start", Linker::OInitArray, false},
				{"__preinit_array_end", Linker::OInitArray, false},
				{"__fini_array_start", Linker::OFiniArray, false},
				{"__fini_array_end", Linker::OFiniArray, true},
				{"__bss_start", Linker::OBss, false},
				{"_edata", Linker::OBss, false},
				{"edata", Linker::OBss, false},
				{"_end", Linker::OBss, true},
				{"end", Linker::OBss, true},
				{"__end__", Linker::OBss, true},
				{"__ehdr_start", Linker::kOutSecs, false},
				{"_DYNAMIC", Linker::ODynamic, false},
				{"__data_start", Linker::OData, false},
				{"data_start", Linker::OData, false},
				{"__TMC_END__", Linker::OData, false},
		};

		B32 isLinkerSym(const String& n) {
			for(const LinkerSym& s : kLinkerSyms)
				if(n == s.name)
					return true;
			return false;
		}

		// ':'-join for DT_RUNPATH
		String joinColon(const List<String>& v) {
			String s;
			for(U32 i = 0; i < v.size(); ++i) {
				if(i)
					s += ':';
				s += v[i];
			}
			return s;
		}

		void skipLeb(const U8* p, U64& i) {
			while(p[i++] & 0x80) {
			}
		}

		// byte size of a DW_EH_PE value format, absptr is 8 on x86-64
		U32 encSize(U8 enc) {
			switch(enc & 0x07) {
			case 0x02:
				return 2;
			case 0x03:
				return 4;
			}
			return 8;
		}

		// the fde pointer encoding of the cie at idOff from its 'R' augmentation, absptr without
		U8 cieFdeEncoding(const List<U8>& ehFrame, U64 idOff) {
			U64 i = idOff + 5; // past the id and version
			const C8* aug = (const C8*)&ehFrame[i];
			U64 augLen = strlen(aug);
			i += augLen + 1;
			skipLeb(ehFrame.data(), i); // code align
			skipLeb(ehFrame.data(), i); // data align
			skipLeb(ehFrame.data(), i); // ra reg
			if(aug[0] != 'z')
				return 0;
			skipLeb(ehFrame.data(), i); // aug data len
			for(U64 a = 1; a < augLen; ++a) {
				if(aug[a] == 'R')
					return ehFrame[i];
				if(aug[a] == 'L')
					++i; // lsda enc byte
				else if(aug[a] == 'P')
					i += 1 + encSize(ehFrame[i]); // personality enc and ptr
			}
			return 0;
		}

		U64 fdeInitialLocation(const List<U8>& ehFrame, U64 pcOff, U8 enc, U64 ehFrameVaddr) {
			U32 bytes = encSize(enc);
			I64 value = (I64)le::rd(&ehFrame[pcOff], bytes);
			if(enc & 0x08) // signed
				value = signExtend(value, bytes * 8);
			if((enc & 0x70) == 0x10) // pcrel
				return ehFrameVaddr + pcOff + value;
			return (U64)value; // absolute, filled by a reloc
		}

		// Elf64_Phdr, paddr = vaddr
		void putProgramHeader(List<U8>& out, const ProgramHeader& ph) {
			le::put32(out, ph.type);
			le::put32(out, ph.flags);
			le::put64(out, ph.offset);
			le::put64(out, ph.vaddr);
			le::put64(out, ph.vaddr);
			le::put64(out, ph.fileSize);
			le::put64(out, ph.memSize);
			le::put64(out, ph.align);
		}

		void putDynamic(List<U8>& dynamic, I64 tag, U64 value) {
			le::put64(dynamic, (U64)tag);
			le::put64(dynamic, value);
		}
	} // namespace detail

	// (initial_location, fde) vaddrs, the locations are final once relocs are applied
	List<Pair<U64, U64>> Linker::fdeTable() const {
		const List<U8>& m = merged[BEhFrame];
		U64 ehVa = vaddr[OEhFrame];
		List<Pair<U64, U64>> table;
		U64 off = 0;
		while(off + 4 <= m.size()) {
			U32 len = le::rd32(&m[off]);
			if(len == 0) {
				off += 4;
				continue;
			}
			if(len == 0xffffffff)
				break;
			U64 idOff = off + 4;
			U32 id = le::rd32(&m[idOff]);
			if(id != 0 && id <= idOff) { // an fde, its cie starts id bytes back
				U8 enc = detail::cieFdeEncoding(m, idOff - id + 4);
				table.push_back({detail::fdeInitialLocation(m, idOff + 4, enc, ehVa), ehVa + off});
			}
			off = idOff + len;
		}
		return table;
	}

	// header and the pc sorted table for bsearch
	void Linker::buildEhFrameHdr() {
		U64 ehVa = vaddr[OEhFrame];
		U64 hdrVa = vaddr[OEhFrameHdr];
		List<Pair<U64, U64>> table = fdeTable();
		std::sort(table.begin(), table.end());

		// version, eh_frame_ptr pcrel sdata4, fde_count udata4, table datarel sdata4
		ehFrameHdr = {1, 0x1b, 0x03, 0x3b};
		le::put32(ehFrameHdr, (U32)(ehVa - (hdrVa + 4)));
		le::put32(ehFrameHdr, (U32)table.size());
		for(const auto& [pc, fde] : table) {
			le::put32(ehFrameHdr, (U32)(pc - hdrVa));
			le::put32(ehFrameHdr, (U32)(fde - hdrVa));
		}
	}

	// append s to its same-named debug output, keep input align
	void Linker::mergeDebug(const InObject& obj, InSection& s) {
		auto [it, fresh] = debugOutIndex.emplace(s.name, (U32)debugOuts.size());
		if(fresh)
			debugOuts.push_back({s.name, {}, 1});
		s.debugOut = it->second;
		DebugOut& d = debugOuts[s.debugOut];
		d.align = std::max(d.align, s.align);
		le::padTo(d.bytes, alignUp(d.bytes.size(), s.align));
		s.outOff = d.bytes.size();
		const U8* p = &obj.image[s.fileOff];
		d.bytes.insert(d.bytes.end(), p, p + s.size);
	}

	void Linker::layout() {
		fillBuckets();
		sizeSections();
		placeSections();
		placeImports();
		placeGlobals();
		defineLinkerSyms();
	}

	// kept sections concatenated per bucket, _start last in .text, commons at the end of .bss
	void Linker::fillBuckets() {
		for(InObject& obj : objs) {
			for(InSection& s : obj.sections) {
				if(!s.keep)
					continue;
				if(s.bucket == BDebug) {
					mergeDebug(obj, s);
					continue;
				}
				U8 b = s.bucket;
				bucketAlign[b] = std::max(bucketAlign[b], s.align);
				if(b == BBss || b == BTbss) {
					bucketSize[b] = alignUp(bucketSize[b], s.align);
					s.outOff = bucketSize[b];
					bucketSize[b] += s.size;
				} else {
					le::padTo(merged[b], alignUp(merged[b].size(), s.align));
					s.outOff = merged[b].size();
					const U8* p = &obj.image[s.fileOff];
					merged[b].insert(merged[b].end(), p, p + s.size);
					bucketSize[b] = merged[b].size();
				}
			}
		}
		List<U8>& text = merged[BText];
		le::padTo(text, alignUp(text.size(), 16));
		text.insert(text.end(), std::begin(detail::kStartCode), std::end(detail::kStartCode));
		bucketSize[BText] = text.size();

		// zero-length record terminates .eh_frame
		if(!merged[BEhFrame].empty()) {
			merged[BEhFrame].insert(merged[BEhFrame].end(), 4, 0);
			bucketSize[BEhFrame] = merged[BEhFrame].size();
		}

		for(auto& [name, d] : globals) {
			if(!d.common)
				continue;
			bucketAlign[BBss] = std::max(bucketAlign[BBss], d.comAlign);
			bucketSize[BBss] = alignUp(bucketSize[BBss], d.comAlign);
			d.addr = bucketSize[BBss]; // bucket-relative, absolutized in placeGlobals
			bucketSize[BBss] += d.comSize ? d.comSize : 1;
		}
	}

	void Linker::sizeSections() {
		U32 nFunc = 0, nData = 0, nGlob = 0;
		for(const Import& im : imports)
			(im.kind == Import::Func ? nFunc : nData)++;
		for(const GotSlot& g : gotSlots)
			nGlob += g.isImport ? 1 : 0;
		U32 nDyn = 1 + (U32)imports.size();
		for(U32 b = 0; b < BucketCount; ++b)
			size[detail::kBucketSecs[b]] = bucketSize[b];

		size[OInterp] = interp.size() + 1;
		size[OHash] = (U64)(2 + nDyn + nDyn) * 4;
		size[ODynsym] = (U64)nDyn * 24;
		size[ORelaDyn] = (U64)(nData + nGlob) * 24; // COPY + GLOB_DAT
		size[ORelaPlt] = (U64)nFunc * 24;
		size[OPlt] = (U64)nFunc * detail::kPltEntSize;
		U64 fdes = fdeTable().size();
		size[OEhFrameHdr] = fdes ? 12 + fdes * 8 : 0;
		size[OGot] = (U64)gotSlots.size() * 8;
		size[OGotPlt] = (U64)(3 + nFunc) * 8;

		size[ODynstr] = 1;
		for(const String& nm : importNames)
			size[ODynstr] += nm.size() + 1;
		for(const String& lib : neededLibs)
			size[ODynstr] += lib.size() + 1;
		if(!rpaths.empty())
			size[ODynstr] += detail::joinColon(rpaths).size() + 1;

		U32 tags = (U32)neededLibs.size() + (rpaths.empty() ? 0 : 1);
		tags += 6;											 // HASH STRTAB SYMTAB STRSZ SYMENT PLTGOT
		tags += nFunc ? 3 : 0;					 // PLTRELSZ PLTREL JMPREL
		tags += (nData + nGlob) ? 3 : 0; // RELA RELASZ RELAENT
		tags += size[OInitArray] ? 2 : 0;
		tags += size[OFiniArray] ? 2 : 0;
		tags += 3; // FLAGS FLAGS_1 NULL
		tags += 4; // slack
		size[ODynamic] = (U64)tags * 16;
	}

	void Linker::place(OutSec s, U64 align, U64& cursor) {
		cursor = alignUp(cursor, align);
		foff[s] = cursor;
		vaddr[s] = detail::kImageBase + cursor;
		cursor += size[s];
	}

	// an R-X segment identity mapped from file offset 0, an R-W one on a fresh page, nobits last
	void Linker::placeSections() {
		U64 cursor = 64 + (U64)detail::kPhnum * 56;
		place(OInterp, 1, cursor);
		place(OHash, 8, cursor);
		place(ODynsym, 8, cursor);
		place(ODynstr, 1, cursor);
		place(ORelaDyn, 8, cursor);
		place(ORelaPlt, 8, cursor);
		place(OPlt, 16, cursor);
		place(OText, std::max<U64>(bucketAlign[BText], 16), cursor);
		place(ORodata, std::max<U64>(bucketAlign[BRodata], 16), cursor);
		place(OEhFrame, 8, cursor);
		place(OEhFrameHdr, 4, cursor);

		cursor = alignUp(cursor, detail::kPage);
		place(OInitArray, 8, cursor);
		place(OFiniArray, 8, cursor);
		place(OData, std::max<U64>(bucketAlign[BData], 16), cursor);
		place(OTdata, 16, cursor);
		place(OGot, 8, cursor);
		place(OGotPlt, 8, cursor);
		place(ODynamic, 8, cursor);

		cursor = alignUp(cursor, std::max<U64>(bucketAlign[BBss], 16));
		vaddr[OBss] = detail::kImageBase + cursor;
		// tls bss is loader-managed, park after bss
		vaddr[OTbss] = detail::kImageBase + alignUp(cursor + size[OBss], 16);
	}

	// plt stubs for func imports, then COPY slots in .bss for data imports
	void Linker::placeImports() {
		U32 dynIndex = 1;
		U32 pltIndex = 0;
		for(Import::Kind kind : {Import::Func, Import::Data}) {
			for(Import& im : imports) {
				if(im.kind != kind)
					continue;
				im.dynIndex = dynIndex++;
				if(kind == Import::Func) {
					im.pltIndex = pltIndex++;
					im.addr = vaddr[OPlt] + (U64)im.pltIndex * detail::kPltEntSize;
				} else {
					U64 bytes = im.size ? im.size : 8;
					U64 align = bytes >= 16 ? 16 : (bytes >= 8 ? 8 : (bytes >= 4 ? 4 : 1));
					size[OBss] = alignUp(size[OBss], align);
					im.addr = vaddr[OBss] + size[OBss];
					size[OBss] += bytes;
				}
			}
		}
	}

	// commons into .bss, own defs at their placed section
	void Linker::placeGlobals() {
		for(auto& [name, d] : globals) {
			if(d.common) {
				d.addr += vaddr[OBss];
			} else if(d.defined && d.obj != 0xffffffffu) {
				const InObject& obj = objs[d.obj];
				const InSym& s = obj.syms[d.sym];
				d.addr = placedAddr(obj.sections[s.shndx], s.value, d.isTls);
			}
		}
	}

	void Linker::defineLinkerSyms() {
		for(const detail::LinkerSym& s : detail::kLinkerSyms) {
			U64 addr = detail::kImageBase;
			if(s.sec != kOutSecs)
				addr = vaddr[s.sec] + (s.atEnd ? size[s.sec] : 0);
			linkerSyms[s.name] = addr;
		}
	}

	// discarded or undefined targets read as 0, never an error
	void Linker::applyDebugReloc(const InObject& obj, const InRel& r) {
		const InSection& rsec = obj.sections[r.secIdx];
		U8* patch = &debugOuts[rsec.debugOut].bytes[rsec.outOff + r.offset];
		U64 S = 0;
		B32 isFunc = false, isTls = false;
		U64 value = 0;
		if(symbolTarget(obj, r.sym, S, isFunc, isTls))
			value = (U64)((I64)S + r.addend);
		else
			err.clear();
		// other types left as is
		switch(r.type) {
		case R_X86_64_64:
		case R_X86_64_DTPOFF64:
			le::wr(patch, value, 8);
			break;
		case R_X86_64_32:
		case R_X86_64_DTPOFF32:
			le::wr(patch, value, 4);
			break;
		}
	}

	// tls is block-relative, debug sits at vaddr 0
	U64 Linker::placedAddr(const InSection& sec, U64 value, B32 isTls) const {
		if(isTls || sec.bucket == BDebug)
			return sec.outOff + value;
		return vaddr[detail::kBucketSecs[sec.bucket]] + sec.outOff + value;
	}

	B32 Linker::symbolTarget(const InObject& obj, U32 symIdx, U64& addr, B32& isFunc, B32& isTls) {
		const InSym& s = obj.syms[symIdx];
		isTls = s.type == STT_TLS;
		if(s.abs) {
			addr = s.value;
			isFunc = false;
			return true;
		}
		if(!s.undef && s.bind == STB_LOCAL && !s.common) {
			const InSection& sec = obj.sections[s.shndx];
			addr = placedAddr(sec, s.value, isTls);
			isFunc = s.type == STT_FUNC || s.type == STT_GNU_IFUNC;
			return true;
		}
		auto g = globals.find(s.name);
		if(g != globals.end() && g->second.defined) {
			addr = g->second.addr;
			isFunc = g->second.isFunc;
			isTls = g->second.isTls;
			return true;
		}
		auto im = importIndex.find(s.name);
		if(im != importIndex.end()) {
			const Import& imp = imports[im->second];
			addr = imp.addr;
			isFunc = imp.kind == Import::Func;
			return true;
		}
		auto ls = linkerSyms.find(s.name);
		if(ls != linkerSyms.end()) {
			addr = ls->second;
			isFunc = false;
			return true;
		}
		if(s.bind == STB_WEAK) {
			addr = 0;
			isFunc = false;
			return true;
		}
		err = "unresolved symbol '" + s.name + "'";
		return false;
	}

	B32 Linker::applyRelocs() {
		U64 tlsSize = alignUp(size[OTdata] + size[OTbss], 16);
		for(U32 oi = 0; oi < objs.size(); ++oi)
			for(const InRel& r : objs[oi].rels)
				if(!applyReloc(oi, r, tlsSize))
					return false;
		return patchStart();
	}

	B32 Linker::applyReloc(U32 oi, const InRel& r, U64 tlsSize) {
		const InObject& obj = objs[oi];
		const InSection& rsec = obj.sections[r.secIdx];
		U8 b = rsec.bucket;
		if(b == BBss || b == BTbss)
			return true; // nobits carries no relocs
		if(b == BDebug) {
			applyDebugReloc(obj, r);
			return true;
		}
		U64 patchOff = rsec.outOff + r.offset;
		U8* patch = &merged[b][patchOff];
		U64 P = vaddr[detail::kBucketSecs[b]] + patchOff;
		U64 S = 0;
		B32 isFunc = false, isTls = false;
		if(!symbolTarget(obj, r.sym, S, isFunc, isTls))
			return false;
		I64 A = r.addend;
		switch(r.type) {
		case R_X86_64_64:
			le::wr(patch, (U64)((I64)S + A), 8);
			break;
		case R_X86_64_PC32:
		case R_X86_64_PLT32:
		case R_X86_64_GOT32: // pc-rel to symbol (small model)
			le::wr(patch, (U32)(I32)((I64)S + A - (I64)P), 4);
			break;
		case R_X86_64_PC64:
			le::wr(patch, (U64)((I64)S + A - (I64)P), 8);
			break;
		case R_X86_64_32:
			le::wr(patch, (U32)((I64)S + A), 4);
			break;
		case R_X86_64_32S:
			le::wr(patch, (U32)(I32)((I64)S + A), 4);
			break;
		case R_X86_64_16:
			le::wr(patch, (U16)((I64)S + A), 2);
			break;
		case R_X86_64_SIZE32:
			le::wr(patch, (U32)((I64)obj.syms[r.sym].size + A), 4);
			break;
		case R_X86_64_SIZE64:
			le::wr(patch, (U64)((I64)obj.syms[r.sym].size + A), 8);
			break;
		case R_X86_64_GOTPCREL:
		case R_X86_64_GOTPCRELX:
		case R_X86_64_REX_GOTPCRELX: {
			U32 slot = gotIndex[gotKey(oi, r)];
			if(!gotSlots[slot].isImport)
				gotSlots[slot].addr = S; // defined, the slot holds the address
			U64 gAddr = vaddr[OGot] + (U64)slot * 8;
			le::wr(patch, (U32)(I32)((I64)gAddr + A - (I64)P), 4);
			break;
		}
		case R_X86_64_TPOFF32:
			// local-exec, neg offset from tp (end of tls block), S is block-relative
			le::wr(patch, (U32)(I32)((I64)S + A - (I64)tlsSize), 4);
			break;
		case R_X86_64_GOTTPOFF: {
			// initial-exec, got slot holds the local-exec offset, ref is pc-rel to slot
			U32 slot = gotIndex[gotKey(oi, r)];
			gotSlots[slot].addr = (U64)((I64)S - (I64)tlsSize);
			gotSlots[slot].isImport = false;
			U64 gAddr = vaddr[OGot] + (U64)slot * 8;
			le::wr(patch, (U32)(I32)((I64)gAddr + A - (I64)P), 4);
			break;
		}
		default:
			err = "unsupported relocation type " + std::to_string(r.type) + " in " + obj.path;
			return false;
		}
		return true;
	}

	// _start's lea to the entry and call to __libc_start_main
	B32 Linker::patchStart() {
		auto g = globals.find(opt.entry);
		if(g == globals.end() || !g->second.defined) {
			err = "entry symbol '" + opt.entry + "' is undefined";
			return false;
		}
		U64 startOff = size[OText] - sizeof(detail::kStartCode);
		U64 startVaddr = vaddr[OText] + startOff;
		U64 lsmAddr = imports[importIndex["__libc_start_main"]].addr;
		U64 mainDisp = g->second.addr - (startVaddr + detail::kStartMainDisp + 4);
		U64 callDisp = lsmAddr - (startVaddr + detail::kStartCallDisp + 4);
		le::wr(&merged[BText][startOff + detail::kStartMainDisp], mainDisp, 4);
		le::wr(&merged[BText][startOff + detail::kStartCallDisp], callDisp, 4);
		return true;
	}

	// headers for kShdrSections and the debug sections, debug bytes go last
	void Linker::emitShdrs(List<U8>& out) {
		List<OutSection> secs(1); // null entry
		List<U8> names(1, 0);
		for(const detail::OutSectionInfo& kind : detail::kShdrSections) {
			if(!size[kind.sec])
				continue;
			SectionHeader sh = headerOf(kind.info, names);
			sh.addr = vaddr[kind.sec];
			sh.offset = vaddr[kind.sec] - detail::kImageBase; // identity mapped, also for nobits
			sh.size = size[kind.sec];
			if(kind.sec == ODynamic)
				sh.link = 1; // .dynstr, never empty
			secs.push_back({sh});
		}
		for(const DebugOut& d : debugOuts)
			secs.push_back({headerOf({d.name.c_str(), SHT_PROGBITS, 0, d.align, 0}, names), &d.bytes});
		secs.push_back({headerOf({".shstrtab", SHT_STRTAB, 0, 1, 0}, names), &names});
		appendSections(out, secs, (U16)(secs.size() - 1));
	}

	// imports as undefined funcs or defined COPY data, names appended to dynstr
	List<U8> Linker::dynsymTable(List<U8>& dynstr) const {
		const U16 kDefinedShndx = 1;
		List<U8> dynsym((1 + imports.size()) * 24, 0);
		for(U32 i = 0; i < imports.size(); ++i) {
			const Import& im = imports[i];
			U8* p = &dynsym[(U64)im.dynIndex * 24];
			le::wr(p, appendName(dynstr, importNames[i]), 4);
			if(im.kind == Import::Func) {
				p[4] = STB_GLOBAL << 4 | STT_FUNC;
				continue;
			}
			p[4] = STB_GLOBAL << 4 | STT_OBJECT;
			le::wr(p + 6, kDefinedShndx, 2);
			le::wr(p + 8, im.addr, 8);
			le::wr(p + 16, im.size, 8);
		}
		return dynsym;
	}

	List<U8> Linker::hashTable() const {
		U32 nDyn = 1 + (U32)imports.size();
		List<U32> buckets(nDyn, 0), chain(nDyn, 0);
		for(U32 i = 0; i < imports.size(); ++i) {
			U32 di = imports[i].dynIndex;
			U32 h = sysvHash(importNames[i].c_str()) % nDyn;
			chain[di] = buckets[h];
			buckets[h] = di;
		}
		List<U8> hash;
		le::put32(hash, nDyn); // nbucket
		le::put32(hash, nDyn); // nchain
		for(U32 b : buckets)
			le::put32(hash, b);
		for(U32 c : chain)
			le::put32(hash, c);
		return hash;
	}

	// a jmp [rip+disp32] stub per func import to its .got.plt slot, the slot bound by a JUMP_SLOT
	List<U8> Linker::pltStubs(List<U8>& relaPlt) const {
		List<U8> plt;
		for(const Import& im : imports) {
			if(im.kind != Import::Func)
				continue;
			U64 entry = vaddr[OPlt] + im.pltIndex * detail::kPltEntSize;
			U64 gotSlot = vaddr[OGotPlt] + (3 + im.pltIndex) * 8;
			plt.insert(plt.end(), {0xff, 0x25});
			le::put32(plt, (U32)(gotSlot - (entry + 6)));
			plt.insert(plt.end(), detail::kPltEntSize - 6, 0x90);
			le::put64(relaPlt, gotSlot);
			le::put64(relaPlt, ((U64)im.dynIndex << 32) | R_X86_64_JUMP_SLOT);
			le::put64(relaPlt, 0);
		}
		return plt;
	}

	// COPY relocs for data imports, then defined slots hold their address and imports get a GLOB_DAT
	List<U8> Linker::gotTable(List<U8>& relaDyn) const {
		for(const Import& im : imports) {
			if(im.kind != Import::Data)
				continue;
			le::put64(relaDyn, im.addr);
			le::put64(relaDyn, ((U64)im.dynIndex << 32) | R_X86_64_COPY);
			le::put64(relaDyn, 0);
		}
		List<U8> got;
		for(U32 s = 0; s < gotSlots.size(); ++s) {
			const GotSlot& g = gotSlots[s];
			le::put64(got, g.addr); // 0 for imports
			if(!g.isImport)
				continue;
			// g.dynIndex holds the import list index from assignGot
			le::put64(relaDyn, vaddr[OGot] + (U64)s * 8);
			le::put64(relaDyn, ((U64)imports[g.dynIndex].dynIndex << 32) | R_X86_64_GLOB_DAT);
			le::put64(relaDyn, 0);
		}
		return got;
	}

	// lib and runpath names appended to dynstr
	List<U8> Linker::dynamicTable(List<U8>& dynstr, U64 relaDynSize) const {
		List<U8> dyn;
		for(const String& lib : neededLibs)
			detail::putDynamic(dyn, DT_NEEDED, appendName(dynstr, lib));
		if(!rpaths.empty())
			detail::putDynamic(dyn, DT_RUNPATH, appendName(dynstr, detail::joinColon(rpaths)));
		detail::putDynamic(dyn, DT_HASH, vaddr[OHash]);
		detail::putDynamic(dyn, DT_STRTAB, vaddr[ODynstr]);
		detail::putDynamic(dyn, DT_SYMTAB, vaddr[ODynsym]);
		detail::putDynamic(dyn, DT_STRSZ, dynstr.size());
		detail::putDynamic(dyn, DT_SYMENT, 24);
		detail::putDynamic(dyn, DT_PLTGOT, vaddr[OGotPlt]);
		if(size[ORelaPlt]) {
			detail::putDynamic(dyn, DT_PLTRELSZ, size[ORelaPlt]);
			detail::putDynamic(dyn, DT_PLTREL, DT_RELA);
			detail::putDynamic(dyn, DT_JMPREL, vaddr[ORelaPlt]);
		}
		if(relaDynSize) {
			detail::putDynamic(dyn, DT_RELA, vaddr[ORelaDyn]);
			detail::putDynamic(dyn, DT_RELASZ, relaDynSize);
			detail::putDynamic(dyn, DT_RELAENT, 24);
		}
		if(size[OInitArray]) {
			detail::putDynamic(dyn, DT_INIT_ARRAY, vaddr[OInitArray]);
			detail::putDynamic(dyn, DT_INIT_ARRAYSZ, size[OInitArray]);
		}
		if(size[OFiniArray]) {
			detail::putDynamic(dyn, DT_FINI_ARRAY, vaddr[OFiniArray]);
			detail::putDynamic(dyn, DT_FINI_ARRAYSZ, size[OFiniArray]);
		}
		detail::putDynamic(dyn, DT_FLAGS, DF_BIND_NOW);
		detail::putDynamic(dyn, DT_FLAGS_1, DF_1_NOW);
		detail::putDynamic(dyn, DT_NULL, 0);
		le::padTo(dyn, size[ODynamic]);
		return dyn;
	}

	detail::ProgramHeader Linker::segmentOf(U32 type, U32 flags, OutSec s, U64 align) const {
		return {type, flags, foff[s], vaddr[s], size[s], size[s], align};
	}

	// the section header fields are patched by appendSections
	void Linker::putHeaders(List<U8>& out) const {
		U64 entry = vaddr[OText] + size[OText] - sizeof(detail::kStartCode);
		putFileHeader(out, ET_EXEC, entry, detail::kPhnum);
		U64 phdrSize = (U64)detail::kPhnum * 56;
		U64 rxSize = foff[OEhFrameHdr] + size[OEhFrameHdr];
		U64 rwFileSize = foff[ODynamic] + size[ODynamic] - foff[OInitArray];
		U64 rwMemSize = vaddr[OBss] + size[OBss] - vaddr[OInitArray];
		U64 tlsMemSize = size[OTdata] + size[OTbss];
		detail::ProgramHeader ehFrame = {PT_NOTE, PF_R, 0, 0, 0, 0, 4}; // filler to keep phnum fixed
		if(size[OEhFrameHdr])
			ehFrame = segmentOf(PT_GNU_EH_FRAME, PF_R, OEhFrameHdr, 4);
		const detail::ProgramHeader phdrs[] = {
				{PT_PHDR, PF_R, 64, detail::kImageBase + 64, phdrSize, phdrSize, 8},
				segmentOf(PT_INTERP, PF_R, OInterp, 1),
				{PT_LOAD, PF_R | PF_X, 0, detail::kImageBase, rxSize, rxSize, detail::kPage},
				{PT_LOAD,
				 PF_R | PF_W,
				 foff[OInitArray],
				 vaddr[OInitArray],
				 rwFileSize,
				 rwMemSize,
				 detail::kPage},
				segmentOf(PT_DYNAMIC, PF_R | PF_W, ODynamic, 8),
				{PT_TLS, PF_R, foff[OTdata], vaddr[OTdata], size[OTdata], tlsMemSize, 16},
				ehFrame,
				{PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 16},
		};
		static_assert(sizeof(phdrs) / sizeof(phdrs[0]) == detail::kPhnum, "one entry per phdr");
		for(const detail::ProgramHeader& ph : phdrs)
			detail::putProgramHeader(out, ph);
	}

	B32 Linker::write() {
		// dynsym appends the import names to dynstr before the dynamic table adds the rest and its size
		List<U8> dynstr(1, 0);
		List<U8> dynsym = dynsymTable(dynstr);
		List<U8> relaDyn;
		List<U8> got = gotTable(relaDyn);
		List<U8> dynamic = dynamicTable(dynstr, relaDyn.size());
		List<U8> interpBytes;
		le::putString(interpBytes, interp);
		List<U8> hash = hashTable();
		List<U8> relaPlt;
		List<U8> plt = pltStubs(relaPlt);
		List<U8> gotPlt; // .dynamic, then the reserved and the lazily bound slots
		le::put64(gotPlt, vaddr[ODynamic]);
		le::padTo(gotPlt, size[OGotPlt]);
		// file contents per OutSec up to the nobits
		const List<U8>* contents[] = {&interpBytes,
																	&hash,
																	&dynsym,
																	&dynstr,
																	&relaDyn,
																	&relaPlt,
																	&plt,
																	&merged[BText],
																	&merged[BRodata],
																	&merged[BEhFrame],
																	&ehFrameHdr,
																	&merged[BInitArray],
																	&merged[BFiniArray],
																	&merged[BData],
																	&merged[BTdata],
																	&got,
																	&gotPlt,
																	&dynamic};
		static_assert(sizeof(contents) / sizeof(contents[0]) == OBss, "one entry per file section");

		List<U8> out;
		out.reserve(foff[ODynamic] + size[ODynamic] + 1024);
		putHeaders(out);
		for(U32 s = 0; s < OBss; ++s) {
			le::padTo(out, foff[s]);
			out.insert(out.end(), contents[s]->begin(), contents[s]->end());
		}
		emitShdrs(out);
		return writeFile(out);
	}

	B32 Linker::writeFile(const List<U8>& out) {
		std::ofstream os(opt.output, std::ios::binary | std::ios::trunc);
		if(!os) {
			err = "cannot write '" + opt.output + "'";
			return false;
		}
		os.write((const C8*)out.data(), (std::streamsize)out.size());
		os.close();
		if(!os) {
			err = "write to '" + opt.output + "' failed";
			return false;
		}
		chmod(opt.output.c_str(), 0755);
		return true;
	}
} // namespace rat
