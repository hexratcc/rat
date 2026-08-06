#include "linker_internal.h"

#include "elf_file.h"

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
	void Linker::buildEhFrameHdr() {
		ehFrameHdr.clear();
		const List<U8>& m = merged[BEhFrame];
		U64 ehVa = vaddr[OEhFrame];
		U64 hdrVa = vaddr[OEhFrameHdr];

		Map<U64, U8> cieFdeEnc;					 // cie off -> fde ptr enc
		List<std::pair<U64, U64>> table; // (initial_location va, fde va)

		U64 off = 0;
		while(off + 4 <= m.size()) {
			U32 len = rd32(&m[off]);
			if(len == 0) {
				off += 4;
				continue;
			}
			if(len == 0xffffffff)
				break;
			U64 recStart = off;
			U64 idOff = off + 4;
			U32 id = rd32(&m[idOff]);
			U64 recEnd = idOff + len;
			if(id == 0) {
				// cie: parse augmentation for 'R' fde ptr enc
				U64 i = idOff + 4;
				U8 ver = m[i++];
				(void)ver;
				const char* aug = (const char*)&m[i];
				U64 augLen = 0;
				while(m[i + augLen])
					++augLen;
				U64 augStart = i;
				i += augLen + 1;
				readUleb(m.data(), i); // code align
				readSleb(m.data(), i); // data align
				readUleb(m.data(), i); // ra reg
				U8 fdeEnc = 0x00;			 // absptr default
				if(aug[0] == 'z') {
					readUleb(m.data(), i); // aug data len
					for(U64 a = 1; a < augLen; ++a) {
						char c = ((const char*)&m[augStart])[a];
						if(c == 'L') {
							++i; // lsda enc byte
						} else if(c == 'P') {
							U8 penc = m[i++];
							i += encSize(penc); // personality ptr
						} else if(c == 'R') {
							fdeEnc = m[i++];
						} else if(c == 'S') {
							// signal frame, no data
						}
					}
				}
				cieFdeEnc[recStart] = fdeEnc;
			} else {
				// fde: cie ptr = idOff - id
				U64 cieStart = idOff - id;
				U8 fdeEnc = 0x00;
				auto it = cieFdeEnc.find(cieStart);
				if(it != cieFdeEnc.end())
					fdeEnc = it->second;
				U64 pcOff = idOff + 4; // offset in .eh_frame
				U64 pcFieldVa = ehVa + pcOff;
				I64 raw = 0;
				U32 sz = encSize(fdeEnc);
				if((fdeEnc & 0x0f) == 0x0b || (fdeEnc & 0x0f) == 0x0a || (fdeEnc & 0x0f) == 0x0c)
					raw = (I64)signExtend((I64)rd64(&m[pcOff]) & ((sz == 4) ? 0xffffffffull : ~0ull), sz * 8);
				else if(sz == 4)
					raw = (I64)(I32)rd32(&m[pcOff]);
				else
					raw = (I64)rd64(&m[pcOff]);
				U64 pcVa;
				if((fdeEnc & 0x70) == 0x10) // pcrel
					pcVa = (U64)((I64)pcFieldVa + raw);
				else
					pcVa = (U64)raw; // absolute (filled by reloc)
				table.push_back({pcVa, ehVa + recStart});
			}
			off = recEnd;
		}

		std::sort(table.begin(), table.end());

		auto put32 = [&](U32 v) {
			for(U32 k = 0; k < 4; ++k)
				ehFrameHdr.push_back((U8)(v >> (k * 8)));
		};
		ehFrameHdr.push_back(1);		// version
		ehFrameHdr.push_back(0x1b); // eh_frame_ptr: pcrel sdata4
		ehFrameHdr.push_back(0x03); // fde_count: udata4
		ehFrameHdr.push_back(0x3b); // table: datarel sdata4
		put32((U32)(I32)((I64)ehVa - (I64)(hdrVa + 4)));
		put32((U32)table.size());
		for(auto& e : table) {
			put32((U32)(I32)((I64)e.first - (I64)hdrVa));
			put32((U32)(I32)((I64)e.second - (I64)hdrVa));
		}
	}

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
		size[OGotPlt] = (U64)(3 + nFunc) * 8;
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
			if(isTls)
				addr = sec.outOff + s.value;
			else
				addr = bucketVaddr[sec.bucket] + sec.outOff + s.value;
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
		auto put16 = [&](U8 bucket, U64 off, U16 v) {
			U8* p = &merged[bucket][off];
			p[0] = (U8)v;
			p[1] = (U8)(v >> 8);
		};
		auto put32 = [&](U8 bucket, U64 off, U32 v) {
			U8* p = &merged[bucket][off];
			for(U32 i = 0; i < 4; ++i)
				p[i] = (U8)(v >> (i * 8));
		};
		auto put64 = [&](U8 bucket, U64 off, U64 v) {
			U8* p = &merged[bucket][off];
			for(U32 i = 0; i < 8; ++i)
				p[i] = (U8)(v >> (i * 8));
		};
		U64 tlsBlockSize = size[OTdata] + size[OTbss];
		U64 tlsAlignedSize = (tlsBlockSize + 15) & ~15ull;

		for(U32 oi = 0; oi < objs.size(); ++oi) {
			const InObject& obj = objs[oi];
			for(const InRel& r : obj.rels) {
				const InSection& rsec = obj.sections[r.secIdx];
				U8 b = rsec.bucket;
				if(b == BBss || b == BTbss)
					continue; // nobits carries no relocs
				U64 patchOff = rsec.outOff + r.offset;
				U64 P = bucketVaddr[b] + patchOff;
				U64 S = 0;
				B32 isFunc = false, isTls = false;
				if(!symbolTarget(obj, r.sym, S, isFunc, isTls))
					return false;
				I64 A = r.addend;
				switch(r.type) {
				case R_X86_64_64:
					put64(b, patchOff, (U64)((I64)S + A));
					break;
				case R_X86_64_PC32:
				case R_X86_64_PLT32:
				case R_X86_64_GOT32: // pc-rel to symbol (small model)
					put32(b, patchOff, (U32)(I32)((I64)S + A - (I64)P));
					break;
				case R_X86_64_PC64:
					put64(b, patchOff, (U64)((I64)S + A - (I64)P));
					break;
				case R_X86_64_32:
					put32(b, patchOff, (U32)((I64)S + A));
					break;
				case R_X86_64_32S:
					put32(b, patchOff, (U32)(I32)((I64)S + A));
					break;
				case R_X86_64_16:
					put16(b, patchOff, (U16)((I64)S + A));
					break;
				case R_X86_64_SIZE32:
					put32(b, patchOff, (U32)((I64)obj.syms[r.sym].size + A));
					break;
				case R_X86_64_SIZE64:
					put64(b, patchOff, (U64)((I64)obj.syms[r.sym].size + A));
					break;
				case R_X86_64_GOTPCREL:
				case R_X86_64_GOTPCRELX:
				case R_X86_64_REX_GOTPCRELX: {
					const InSym& s = obj.syms[r.sym];
					String key;
					if(!s.undef && s.bind == STB_LOCAL)
						key = "L:" + std::to_string(oi) + ":" + std::to_string(r.sym);
					else
						key = "G:" + s.name;
					U32 slot = gotIndex[key];
					if(!gotSlots[slot].isImport)
						gotSlots[slot].addr = S; // defined: slot holds addr
					U64 gAddr = vaddr[OGot] + (U64)slot * 8;
					put32(b, patchOff, (U32)(I32)((I64)gAddr + A - (I64)P));
					break;
				}
				case R_X86_64_TPOFF32: {
					// local-exec: neg offset from tp (end of tls block); S is block-relative
					I64 tp = (I64)S + A - (I64)tlsAlignedSize;
					put32(b, patchOff, (U32)(I32)tp);
					break;
				}
				case R_X86_64_GOTTPOFF: {
					// initial-exec: got slot holds tpoff (local-exec offset), ref is pc-rel to slot
					const InSym& s = obj.syms[r.sym];
					String key;
					if(!s.undef && s.bind == STB_LOCAL)
						key = "L:" + std::to_string(oi) + ":" + std::to_string(r.sym);
					else
						key = "G:" + s.name;
					U32 slot = gotIndex[key];
					I64 tp = (I64)S - (I64)tlsAlignedSize;
					gotSlots[slot].addr = (U64)tp;
					gotSlots[slot].defined = true;
					gotSlots[slot].isImport = false;
					U64 gAddr = vaddr[OGot] + (U64)slot * 8;
					put32(b, patchOff, (U32)(I32)((I64)gAddr + A - (I64)P));
					break;
				}
				default:
					err = "unsupported relocation type " + std::to_string(r.type) + " in " + obj.path;
					return false;
				}
			}
		}

		// _start fixups (main + __libc_start_main)
		U64 startVaddr = vaddr[OText] + (size[OText] - startCode.size());
		auto g = globals.find(opt.entry);
		if(g == globals.end() || !g->second.defined) {
			err = "entry symbol '" + opt.entry + "' is undefined";
			return false;
		}
		U64 mainAddr = g->second.addr;
		U64 lsmAddr = imports[importIndex["__libc_start_main"]].addr;
		U64 leaVaddr = startVaddr + startLeaDisp;
		U64 callVaddr = startVaddr + startCallDisp;
		I32 leaDisp = (I32)((I64)mainAddr - (I64)(leaVaddr + 4));
		I32 callDisp = (I32)((I64)lsmAddr - (I64)(callVaddr + 4));
		U64 startFileOff = size[OText] - startCode.size();
		for(U32 i = 0; i < 4; ++i) {
			merged[BText][startFileOff + startLeaDisp + i] = (U8)((U32)leaDisp >> (i * 8));
			merged[BText][startFileOff + startCallDisp + i] = (U8)((U32)callDisp >> (i * 8));
		}
		return true;
	}

	B32 Linker::write() {
		// .dynstr
		List<U8> dynstr;
		dynstr.push_back(0);
		List<U32> importNameOff(imports.size(), 0);
		for(U32 i = 0; i < importNames.size(); ++i) {
			importNameOff[i] = (U32)dynstr.size();
			const String& nm = importNames[i];
			dynstr.insert(dynstr.end(), nm.begin(), nm.end());
			dynstr.push_back(0);
		}
		List<U32> libNameOff(neededLibs.size(), 0);
		for(U32 i = 0; i < neededLibs.size(); ++i) {
			libNameOff[i] = (U32)dynstr.size();
			const String& nm = neededLibs[i];
			dynstr.insert(dynstr.end(), nm.begin(), nm.end());
			dynstr.push_back(0);
		}
		U32 runpathOff = 0;
		if(!rpaths.empty()) {
			runpathOff = (U32)dynstr.size();
			String rp = joinColon(rpaths);
			dynstr.insert(dynstr.end(), rp.begin(), rp.end());
			dynstr.push_back(0);
		}

		// .dynsym
		U32 nDyn = 1 + (U32)imports.size();
		List<U8> dynsym(nDyn * 24, 0);
		auto writeSym = [&](U32 idx, U32 nameOff, U8 bind, U8 type, U16 shndx, U64 value, U64 sz) {
			U8* p = &dynsym[(U64)idx * 24];
			p[0] = (U8)nameOff;
			p[1] = (U8)(nameOff >> 8);
			p[2] = (U8)(nameOff >> 16);
			p[3] = (U8)(nameOff >> 24);
			p[4] = (U8)((bind << 4) | (type & 0xf));
			p[5] = 0;
			p[6] = (U8)shndx;
			p[7] = (U8)(shndx >> 8);
			for(U32 i = 0; i < 8; ++i)
				p[8 + i] = (U8)(value >> (i * 8));
			for(U32 i = 0; i < 8; ++i)
				p[16 + i] = (U8)(sz >> (i * 8));
		};
		const U16 kDefinedShndx = 1;
		for(U32 i = 0; i < imports.size(); ++i) {
			const Import& im = imports[i];
			if(im.kind == Import::Func)
				writeSym(im.dynIndex, importNameOff[i], STB_GLOBAL, STT_FUNC, SHN_UNDEF, 0, 0);
			else
				writeSym(
						im.dynIndex, importNameOff[i], STB_GLOBAL, STT_OBJECT, kDefinedShndx, im.addr, im.size);
		}

		// .hash
		List<U8> hash;
		{
			U32 nbucket = nDyn;
			List<U32> buckets(nbucket, 0);
			List<U32> chain(nDyn, 0);
			for(U32 i = 0; i < imports.size(); ++i) {
				U32 di = imports[i].dynIndex;
				U32 h = sysvHash(importNames[i].c_str()) % nbucket;
				chain[di] = buckets[h];
				buckets[h] = di;
			}
			wr32(hash, nbucket);
			wr32(hash, nDyn);
			for(U32 bkt : buckets)
				wr32(hash, bkt);
			for(U32 c : chain)
				wr32(hash, c);
		}

		// .rela.plt + .got.plt
		List<U8> relaPlt, gotPlt;
		wr64(gotPlt, vaddr[ODynamic]);
		wr64(gotPlt, 0);
		wr64(gotPlt, 0);
		for(U32 i = 0; i < imports.size(); ++i) {
			const Import& im = imports[i];
			if(im.kind != Import::Func)
				continue;
			U64 slot = vaddr[OGotPlt] + (U64)(3 + im.pltIndex) * 8;
			wr64(relaPlt, slot);
			wr64(relaPlt, ((U64)im.dynIndex << 32) | R_X86_64_JUMP_SLOT);
			wr64(relaPlt, 0);
			wr64(gotPlt, 0);
		}

		// .got (defined -> absolute, import -> GLOB_DAT)
		List<U8> got;
		List<U8> relaDyn;
		for(U32 i = 0; i < imports.size(); ++i) {
			const Import& im = imports[i];
			if(im.kind != Import::Data)
				continue;
			wr64(relaDyn, im.addr);
			wr64(relaDyn, ((U64)im.dynIndex << 32) | R_X86_64_COPY);
			wr64(relaDyn, 0);
		}
		for(U32 s = 0; s < gotSlots.size(); ++s) {
			GotSlot& g = gotSlots[s];
			if(g.isImport) {
				// g.dynIndex holds import list index from assignGot
				U32 dynIdx = 0;
				if(g.dynIndex < imports.size())
					dynIdx = imports[g.dynIndex].dynIndex;
				wr64(got, 0);
				U64 slotAddr = vaddr[OGot] + (U64)s * 8;
				wr64(relaDyn, slotAddr);
				wr64(relaDyn, ((U64)dynIdx << 32) | R_X86_64_GLOB_DAT);
				wr64(relaDyn, 0);
			} else {
				wr64(got, g.addr);
			}
		}

		// .plt
		List<U8> plt;
		{
			U32 slotN = 0;
			for(U32 i = 0; i < imports.size(); ++i) {
				if(imports[i].kind != Import::Func)
					continue;
				U64 entry = vaddr[OPlt] + (U64)slotN * kPltEntSize;
				U64 slot = vaddr[OGotPlt] + (U64)(3 + slotN) * 8;
				I32 disp = (I32)((I64)slot - (I64)(entry + 6));
				plt.push_back(0xff);
				plt.push_back(0x25);
				for(U32 k = 0; k < 4; ++k)
					plt.push_back((U8)((U32)disp >> (k * 8)));
				while(plt.size() % kPltEntSize)
					plt.push_back(0x90);
				++slotN;
			}
		}

		// .dynamic
		List<U8> dyn;
		auto dt = [&](I64 tag, U64 val) {
			wr64(dyn, (U64)tag);
			wr64(dyn, val);
		};
		for(U32 i = 0; i < neededLibs.size(); ++i)
			dt(DT_NEEDED, libNameOff[i]);
		if(!rpaths.empty())
			dt(DT_RUNPATH, runpathOff);
		dt(DT_HASH, vaddr[OHash]);
		dt(DT_STRTAB, vaddr[ODynstr]);
		dt(DT_SYMTAB, vaddr[ODynsym]);
		dt(DT_STRSZ, dynstr.size());
		dt(DT_SYMENT, 24);
		dt(DT_PLTGOT, vaddr[OGotPlt]);
		if(size[ORelaPlt]) {
			dt(DT_PLTRELSZ, size[ORelaPlt]);
			dt(DT_PLTREL, DT_RELA);
			dt(DT_JMPREL, vaddr[ORelaPlt]);
		}
		if(!relaDyn.empty()) {
			dt(DT_RELA, vaddr[ORelaDyn]);
			dt(DT_RELASZ, relaDyn.size());
			dt(DT_RELAENT, 24);
		}
		if(size[OInitArray]) {
			dt(DT_INIT_ARRAY, vaddr[OInitArray]);
			dt(DT_INIT_ARRAYSZ, size[OInitArray]);
		}
		if(size[OFiniArray]) {
			dt(DT_FINI_ARRAY, vaddr[OFiniArray]);
			dt(DT_FINI_ARRAYSZ, size[OFiniArray]);
		}
		dt(DT_FLAGS, DF_BIND_NOW);
		dt(DT_FLAGS_1, DF_1_NOW);
		dt(DT_NULL, 0);
		if(dyn.size() < size[ODynamic])
			dyn.insert(dyn.end(), size[ODynamic] - dyn.size(), 0);

		// assemble
		List<U8> out;
		U64 fileEnd = foff[ODynamic] + size[ODynamic];
		out.reserve(fileEnd + 1024);
		auto emitAt = [&](U64 off, const List<U8>& blob) {
			wrPad(out, off);
			out.insert(out.end(), blob.begin(), blob.end());
		};

		U64 startVaddr = vaddr[OText] + (size[OText] - startCode.size());
		U16 phnum = 8;
		const U8 ident[16] = {
				0x7f, 'E', 'L', 'F', ELFCLASS64, ELFDATA2LSB, EV_CURRENT, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		for(U8 c : ident)
			out.push_back(c);
		wr16(out, ET_EXEC);
		wr16(out, EM_X86_64);
		wr32(out, EV_CURRENT);
		wr64(out, startVaddr);
		wr64(out, 64);
		wr64(out, 0);
		wr32(out, 0);
		wr16(out, 64);
		wr16(out, 56);
		wr16(out, phnum);
		wr16(out, 0);
		wr16(out, 0);
		wr16(out, 0);

		U64 rxFilesz = foff[OEhFrameHdr] + size[OEhFrameHdr];
		if(!rxFilesz)
			rxFilesz = foff[ORodata] + size[ORodata];
		U64 rwStart = foff[OInitArray];
		U64 rwFilesz = (foff[ODynamic] + size[ODynamic]) - rwStart;
		U64 rwMemsz = (vaddr[OBss] + size[OBss]) - vaddr[OInitArray];
		auto phdr = [&](U32 type, U32 flags, U64 off, U64 va, U64 filesz, U64 memsz, U64 align) {
			wr32(out, type);
			wr32(out, flags);
			wr64(out, off);
			wr64(out, va);
			wr64(out, va);
			wr64(out, filesz);
			wr64(out, memsz);
			wr64(out, align);
		};
		phdr(PT_PHDR, PF_R, 64, kImageBase + 64, (U64)phnum * 56, (U64)phnum * 56, 8);
		phdr(PT_INTERP, PF_R, foff[OInterp], vaddr[OInterp], size[OInterp], size[OInterp], 1);
		phdr(PT_LOAD, PF_R | PF_X, 0, kImageBase, rxFilesz, rxFilesz, kPage);
		phdr(PT_LOAD, PF_R | PF_W, rwStart, vaddr[OInitArray], rwFilesz, rwMemsz, kPage);
		phdr(PT_DYNAMIC,
				 PF_R | PF_W,
				 foff[ODynamic],
				 vaddr[ODynamic],
				 size[ODynamic],
				 size[ODynamic],
				 8);
		{
			U64 tlsFilesz = size[OTdata];
			U64 tlsMemsz = size[OTdata] + size[OTbss];
			phdr(PT_TLS, PF_R, foff[OTdata], vaddr[OTdata], tlsFilesz, tlsMemsz, 16);
		}
		if(size[OEhFrameHdr])
			phdr(PT_GNU_EH_FRAME,
					 PF_R,
					 foff[OEhFrameHdr],
					 vaddr[OEhFrameHdr],
					 size[OEhFrameHdr],
					 size[OEhFrameHdr],
					 4);
		else
			phdr(PT_NOTE, PF_R, 0, 0, 0, 0, 4); // filler to keep phnum fixed
		phdr(PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 16);

		List<U8> interpBytes(interp.begin(), interp.end());
		interpBytes.push_back(0);
		emitAt(foff[OInterp], interpBytes);
		emitAt(foff[OHash], hash);
		emitAt(foff[ODynsym], dynsym);
		emitAt(foff[ODynstr], dynstr);
		emitAt(foff[ORelaDyn], relaDyn);
		emitAt(foff[ORelaPlt], relaPlt);
		emitAt(foff[OPlt], plt);
		emitAt(foff[OText], merged[BText]);
		emitAt(foff[ORodata], merged[BRodata]);
		emitAt(foff[OEhFrame], merged[BEhFrame]);
		emitAt(foff[OEhFrameHdr], ehFrameHdr);
		emitAt(foff[OInitArray], merged[BInitArray]);
		emitAt(foff[OFiniArray], merged[BFiniArray]);
		emitAt(foff[OData], merged[BData]);
		emitAt(foff[OTdata], merged[BTdata]);
		emitAt(foff[OGot], got);
		emitAt(foff[OGotPlt], gotPlt);
		emitAt(foff[ODynamic], dyn);

		std::ofstream os(opt.output, std::ios::binary | std::ios::trunc);
		if(!os) {
			err = "cannot write '" + opt.output + "'";
			return false;
		}
		os.write((const char*)out.data(), (std::streamsize)out.size());
		os.close();
		if(!os) {
			err = "write to '" + opt.output + "' failed";
			return false;
		}
		if(opt.executable)
			chmod(opt.output.c_str(), 0755);
		return true;
	}
} // namespace rat
