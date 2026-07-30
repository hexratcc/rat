#include "LinkerInternal.h"

#include "ElfFile.h"

#include <fstream>
#include <sys/stat.h>

namespace rat {
	using namespace elf;

	namespace {
		constexpr U64 kImageBase = 0x400000;
		constexpr U64 kPage = 0x1000;
		constexpr U64 kPltEntSize = 16;

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
	} // namespace

	// count FDEs in merged .eh_frame (drives hdr size)
	U32 Linker::countFdes() {
		const List<U8>& m = merged[BEhFrame];
		U32 n = 0;
		U64 off = 0;
		while(off + 4 <= m.size()) {
			U32 len = rd32(&m[off]);
			if(len == 0) {
				off += 4;
				continue; // terminator
			}
			if(len == 0xffffffff)
				break; // 64-bit form unsupported
			U64 idOff = off + 4;
			if(idOff + 4 > m.size())
				break;
			U32 id = rd32(&m[idOff]);
			if(id != 0)
				++n; // FDE (CIE has id 0)
			off = idOff + len;
		}
		return n;
	}

	namespace {
		U64 readUleb(const U8* p, U64& i) {
			U64 v = 0;
			U32 sh = 0;
			U8 b;
			do {
				b = p[i++];
				v |= (U64)(b & 0x7f) << sh;
				sh += 7;
			} while(b & 0x80);
			return v;
		}
		I64 readSleb(const U8* p, U64& i) {
			I64 v = 0;
			U32 sh = 0;
			U8 b;
			do {
				b = p[i++];
				v |= (I64)(b & 0x7f) << sh;
				sh += 7;
			} while(b & 0x80);
			if(sh < 64 && (b & 0x40))
				v |= -((I64)1 << sh);
			return v;
		}
		// byte size of DW_EH_PE value (lower nibble)
		U32 encSize(U8 enc) {
			switch(enc & 0x0f) {
			case 0x00:
				return 8; // absptr = 8 on x86-64
			case 0x02:
				return 2;
			case 0x03:
				return 4;
			case 0x04:
				return 8;
			case 0x0a:
				return 2;
			case 0x0b:
				return 4;
			case 0x0c:
				return 8;
			}
			return 8;
		}
	} // namespace

	// .eh_frame_hdr: header + pc-sorted (initial_location, fde) table for bsearch

	void Linker::layout() {
		// 1. concat kept sections into buckets, assign offsets; _start last in .text
		auto padTo = [](List<U8>& v, U64 a) {
			if(a > 1)
				while(v.size() % a)
					v.push_back(0);
		};
		for(InObject& obj : objs) {
			for(InSection& s : obj.sections) {
				if(!s.keep)
					continue;
				U8 b = s.bucket;
				if(b == BBss || b == BTbss) {
					bucketSize[b] = (bucketSize[b] + (s.align - 1)) & ~(s.align - 1);
					s.outOff = bucketSize[b];
					bucketSize[b] += s.size;
				} else {
					padTo(merged[b], s.align);
					s.outOff = merged[b].size();
					const U8* p = &obj.image[s.fileOff];
					merged[b].insert(merged[b].end(), p, p + s.size);
					bucketSize[b] = merged[b].size();
				}
			}
		}
		padTo(merged[BText], 16);
		merged[BText].insert(merged[BText].end(), startCode.begin(), startCode.end());
		bucketSize[BText] = merged[BText].size();

		// zero-length record terminates .eh_frame
		if(!merged[BEhFrame].empty()) {
			merged[BEhFrame].insert(merged[BEhFrame].end(), 4, 0);
			bucketSize[BEhFrame] = merged[BEhFrame].size();
		}

		// commons -> appended to .bss
		for(auto& kv : globals) {
			Def& d = kv.second;
			if(!d.common)
				continue;
			U64 a = d.comAlign ? d.comAlign : 1;
			bucketSize[BBss] = (bucketSize[BBss] + (a - 1)) & ~(a - 1);
			d.addr = bucketSize[BBss]; // bucket-relative, absolutized below
			bucketSize[BBss] += d.comSize ? d.comSize : 1;
		}

		// 2. section sizes
		U32 nImports = (U32)imports.size();
		U32 nFunc = 0, nData = 0;
		for(const Import& im : imports)
			(im.kind == Import::Func ? nFunc : nData)++;
		U32 nDyn = 1 + nImports;

		size[OInterp] = interp.size() + 1;
		U32 nGlob = 0;
		for(const GotSlot& g : gotSlots)
			nGlob += g.isImport ? 1 : 0;
		size[ODynsym] = (U64)nDyn * 24;
		size[ORelaDyn] = (U64)(nData + nGlob) * 24; // COPY + GLOB_DAT
		size[ORelaPlt] = (U64)nFunc * 24;
		size[OPlt] = (U64)nFunc * kPltEntSize;
		size[OText] = bucketSize[BText];
		size[ORodata] = bucketSize[BRodata];
		size[OEhFrame] = bucketSize[BEhFrame];
		ehFdeCount = size[OEhFrame] ? countFdes() : 0;
		size[OEhFrameHdr] = ehFdeCount ? (12 + (U64)ehFdeCount * 8) : 0;
		size[OInitArray] = bucketSize[BInitArray];
		size[OFiniArray] = bucketSize[BFiniArray];
		size[OData] = bucketSize[BData];
		size[OTdata] = bucketSize[BTdata];
		size[OGot] = (U64)gotSlots.size() * 8;
		size[OGotPlt] = (U64)(2 + nFunc) * 8;
		size[OHash] = (U64)(2 + nDyn + nDyn) * 4;
		{
			U64 n = 1;
			for(const String& nm : importNames)
				n += nm.size() + 1;
			for(const String& lib : neededLibs)
				n += lib.size() + 1;
			if(!rpaths.empty())
				n += joinColon(rpaths).size() + 1;
			size[ODynstr] = n;
		}

		// 3. first R-X segment: identity map from file off 0
		U16 phnum = 8; // PHDR INTERP LOAD LOAD DYNAMIC TLS GNU_EH_FRAME GNU_STACK
		U64 cursor = 64 + (U64)phnum * 56;
		auto place = [&](OutSec s, U64 align) {
			if(align > 1)
				cursor = (cursor + (align - 1)) & ~(align - 1);
			foff[s] = cursor;
			vaddr[s] = kImageBase + cursor;
			cursor += size[s];
		};
		place(OInterp, 1);
		place(OHash, 8);
		place(ODynsym, 8);
		place(ODynstr, 1);
		place(ORelaDyn, 8);
		place(ORelaPlt, 8);
		place(OPlt, 16);
		place(OText, 16);
		place(ORodata, 16);
		place(OEhFrame, 8);
		place(OEhFrameHdr, 4);

		// 4. second R-W segment on fresh page
		cursor = (cursor + kPage - 1) & ~(kPage - 1);
		place(OInitArray, 8);
		place(OFiniArray, 8);
		place(OData, 16);
		place(OTdata, 16);
		place(OGot, 8);
		place(OGotPlt, 8);
		{
			U32 n = (U32)neededLibs.size();
			n += rpaths.empty() ? 0 : 1;
			n += 6;												// HASH STRTAB SYMTAB STRSZ SYMENT PLTGOT
			n += nFunc ? 3 : 0;						// PLTRELSZ PLTREL JMPREL
			n += (nData + nGlob) ? 3 : 0; // RELA RELASZ RELAENT
			n += size[OInitArray] ? 2 : 0;
			n += size[OFiniArray] ? 2 : 0;
			n += 3; // FLAGS FLAGS_1 NULL
			n += 4; // slack
			size[ODynamic] = (U64)n * 16;
		}
		place(ODynamic, 8);

		// nobits last
		cursor = (cursor + 15) & ~15ull;
		vaddr[OBss] = kImageBase + cursor;
		foff[OBss] = cursor;
		size[OBss] = bucketSize[BBss];
		// tls bss is loader-managed, park after bss
		U64 tbssCur = cursor + size[OBss];
		tbssCur = (tbssCur + 15) & ~15ull;
		vaddr[OTbss] = kImageBase + tbssCur;
		foff[OTbss] = tbssCur;
		size[OTbss] = bucketSize[BTbss];

		// 5. bucket vaddrs + final addresses
		for(U32 b = 0; b < BucketCount; ++b)
			bucketVaddr[b] = vaddr[outSecOf((U8)b)];

		// data imports get a COPY slot in .bss (after commons + merged bss)
		U64 copyCursor = size[OBss];
		U32 funcSlot = 0;
		U32 dynCursor = 1;
		for(U32 pass = 0; pass < 2; ++pass) {
			for(U32 i = 0; i < imports.size(); ++i) {
				Import& im = imports[i];
				B32 isFunc = im.kind == Import::Func;
				if((pass == 0) != (B32)isFunc)
					continue;
				im.dynIndex = dynCursor++;
				if(isFunc) {
					im.pltIndex = funcSlot++;
					im.addr = vaddr[OPlt] + (U64)im.pltIndex * kPltEntSize;
				} else {
					U64 a = im.size ? im.size : 8;
					U64 align = a >= 16 ? 16 : (a >= 8 ? 8 : (a >= 4 ? 4 : 1));
					copyCursor = (copyCursor + (align - 1)) & ~(align - 1);
					im.addr = vaddr[OBss] + copyCursor;
					copyCursor += a;
				}
			}
		}
		size[OBss] = copyCursor;

		// absolutize commons
		for(auto& kv : globals) {
			Def& d = kv.second;
			if(d.common)
				d.addr += vaddr[OBss];
		}

		// resolve own global defs (non-common)
		for(auto& kv : globals) {
			Def& d = kv.second;
			if(d.common || !d.defined || d.obj == 0xffffffffu)
				continue;
			const InObject& obj = objs[d.obj];
			const InSym& s = obj.syms[d.sym];
			const InSection& sec = obj.sections[s.shndx];
			if(d.isTls)
				d.addr = sec.outOff + s.value; // tls block offset
			else
				d.addr = bucketVaddr[sec.bucket] + sec.outOff + s.value;
		}

		// synthesized linker/crt symbols
		U64 bssEnd = vaddr[OBss] + size[OBss];
		linkerSyms["__dso_handle"] = vaddr[OData];
		linkerSyms["_GLOBAL_OFFSET_TABLE_"] = vaddr[OGotPlt];
		linkerSyms["__init_array_start"] = vaddr[OInitArray];
		linkerSyms["__init_array_end"] = vaddr[OInitArray] + size[OInitArray];
		linkerSyms["__preinit_array_start"] = vaddr[OInitArray];
		linkerSyms["__preinit_array_end"] = vaddr[OInitArray];
		linkerSyms["__fini_array_start"] = vaddr[OFiniArray];
		linkerSyms["__fini_array_end"] = vaddr[OFiniArray] + size[OFiniArray];
		linkerSyms["__bss_start"] = vaddr[OBss];
		linkerSyms["_edata"] = vaddr[OBss];
		linkerSyms["edata"] = vaddr[OBss];
		linkerSyms["_end"] = bssEnd;
		linkerSyms["end"] = bssEnd;
		linkerSyms["__end__"] = bssEnd;
		linkerSyms["__ehdr_start"] = kImageBase;
		linkerSyms["_DYNAMIC"] = vaddr[ODynamic];
		linkerSyms["__data_start"] = vaddr[OData];
		linkerSyms["data_start"] = vaddr[OData];
		linkerSyms["__TMC_END__"] = vaddr[OData];
	}
} // namespace rat
