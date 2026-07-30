#include "ElfRead.h"

#include <cstring>
#include <fstream>

namespace rat {
	using namespace elf;

	namespace {
		U8 classify(U32 type, U64 flags) {
			if(!(flags & SHF_ALLOC))
				return BNone;
			if(flags & SHF_TLS)
				return type == SHT_NOBITS ? BTbss : BTdata;
			if(type == SHT_INIT_ARRAY)
				return BInitArray;
			if(type == SHT_FINI_ARRAY)
				return BFiniArray;
			if(flags & SHF_EXECINSTR)
				return BText;
			if(type == SHT_NOBITS)
				return BBss;
			if(flags & SHF_WRITE)
				return BData;
			return BRodata;
		}

		struct Shdr {
			U32 name, type, link, info;
			U64 flags, offset, size, addralign, entsize;
		};

		B32 parseShdrs(const List<U8>& img, List<Shdr>& out, const U8*& shstr, String& err) {
			if(img.size() < 64 || img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' || img[3] != 'F') {
				err = "not an ELF file";
				return false;
			}
			if(img[4] != ELFCLASS64 || img[5] != ELFDATA2LSB) {
				err = "not a 64-bit little-endian ELF";
				return false;
			}
			U64 shoff = rd64(&img[40]);
			U16 shentsize = rd16(&img[58]);
			U16 shnum = rd16(&img[60]);
			U16 shstrndx = rd16(&img[62]);
			if(shentsize != 64 || shoff + (U64)shnum * 64 > img.size()) {
				err = "malformed section header table";
				return false;
			}
			out.clear();
			for(U16 i = 0; i < shnum; ++i) {
				const U8* p = &img[shoff + (U64)i * 64];
				Shdr s;
				s.name = rd32(p);
				s.type = rd32(p + 4);
				s.flags = rd64(p + 8);
				s.offset = rd64(p + 24);
				s.size = rd64(p + 32);
				s.link = rd32(p + 40);
				s.info = rd32(p + 44);
				s.addralign = rd64(p + 48);
				s.entsize = rd64(p + 56);
				out.push_back(s);
			}
			if(shstrndx >= out.size()) {
				err = "bad shstrndx";
				return false;
			}
			shstr = &img[out[shstrndx].offset];
			return true;
		}
	} // namespace

	B32 readWhole(const String& path, List<U8>& out) {
		std::ifstream f(path, std::ios::binary);
		if(!f)
			return false;
		f.seekg(0, std::ios::end);
		std::streamoff n = f.tellg();
		if(n < 0)
			return false;
		f.seekg(0, std::ios::beg);
		out.resize((U64)n);
		if(n > 0)
			f.read((char*)out.data(), n);
		return (B32)f.good() || f.eof();
	}

	B32 loadObject(List<U8> img, const String& path, InObject& obj, String& err) {
		List<Shdr> shdrs;
		const U8* shstr = nullptr;
		if(!parseShdrs(img, shdrs, shstr, err))
			return false;
		if(rd16(&img[16]) != ET_REL) {
			err = "'" + path + "' is not a relocatable object";
			return false;
		}
		obj.path = path;
		obj.image = std::move(img);
		const List<U8>& im = obj.image;

		// one InSection per index, syms/relocs reference original indices
		obj.sections.resize(shdrs.size());
		for(U32 i = 0; i < shdrs.size(); ++i) {
			const Shdr& sh = shdrs[i];
			InSection& s = obj.sections[i];
			s.name = (const char*)(shstr + sh.name);
			s.type = sh.type;
			s.flags = sh.flags;
			s.align = sh.addralign ? sh.addralign : 1;
			s.fileOff = sh.offset;
			s.size = sh.size;
			s.bucket = classify(sh.type, sh.flags);
			if(s.bucket == BRodata && s.name == ".eh_frame")
				s.bucket = BEhFrame;
			s.keep = s.bucket != BNone;
		}

		// symbols
		U32 symSec = 0;
		for(U32 i = 0; i < shdrs.size(); ++i)
			if(shdrs[i].type == SHT_SYMTAB)
				symSec = i;
		if(!symSec) {
			err = "'" + path + "' has no symbol table";
			return false;
		}
		const Shdr& sym = shdrs[symSec];
		const U8* strtab = &im[shdrs[sym.link].offset];
		U32 nsym = (U32)(sym.size / 24);
		obj.syms.reserve(nsym);
		for(U32 i = 0; i < nsym; ++i) {
			const U8* p = &im[sym.offset + (U64)i * 24];
			InSym s;
			s.name = (const char*)(strtab + rd32(p));
			U8 info = p[4];
			s.bind = (U8)(info >> 4);
			s.type = (U8)(info & 0xf);
			s.other = (U8)(p[5] & 0x3);
			U16 shndx = rd16(p + 6);
			s.shndx = shndx;
			s.value = rd64(p + 8);
			s.size = rd64(p + 16);
			if(shndx == SHN_UNDEF) {
				s.undef = true;
			} else if(shndx == SHN_ABS) {
				s.undef = false;
				s.abs = true;
			} else if(shndx == SHN_COMMON) {
				s.undef = false;
				s.common = true; // value=align, size=byte count
			} else if(shndx < obj.sections.size() && obj.sections[shndx].keep) {
				s.undef = false;
			} else {
				s.undef = true; // defined in dropped section
			}
			obj.syms.push_back(std::move(s));
		}

		// relocs, keep those patching a kept section
		for(U32 i = 0; i < shdrs.size(); ++i) {
			const Shdr& sh = shdrs[i];
			if(sh.type != SHT_RELA)
				continue;
			if(sh.info >= obj.sections.size() || !obj.sections[sh.info].keep)
				continue;
			U32 n = (U32)(sh.size / 24);
			for(U32 r = 0; r < n; ++r) {
				const U8* p = &im[sh.offset + (U64)r * 24];
				InRel rel;
				rel.secIdx = sh.info;
				rel.offset = rd64(p);
				U64 info = rd64(p + 8);
				rel.sym = (U32)(info >> 32);
				rel.type = (U32)(info & 0xffffffff);
				rel.addend = (I64)rd64(p + 16);
				obj.rels.push_back(rel);
			}
		}
		return true;
	}

	U64 arMemberSize(const List<U8>& d, U64 hdrOff) {
		String s((const char*)&d[hdrOff + 48], 10);
		return (U64)strtoull(s.c_str(), nullptr, 10);
	}

	B32 parseArchive(const String& path, List<U8> bytes, ArchiveFile& ar, String& err) {
		ar.path = path;
		ar.data = std::move(bytes);
		const List<U8>& d = ar.data;
		if(d.size() < 8 || memcmp(d.data(), "!<arch>\n", 8) != 0) {
			err = "not an archive '" + path + "'";
			return false;
		}
		U64 o = 8;
		while(o + 60 <= d.size()) {
			char nm0 = (char)d[o], nm1 = (char)d[o + 1];
			U64 sz = arMemberSize(d, o);
			U64 dataOff = o + 60;
			if(nm0 == '/' && nm1 == ' ') {
				// sysv armap: BE count, BE offsets, NUL names
				const U8* p = &d[dataOff];
				U32 count = (U32)((p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0]);
				const U8* offs = p + 4;
				const char* names = (const char*)(offs + (U64)count * 4);
				U64 nameCursor = 0;
				for(U32 i = 0; i < count; ++i) {
					const U8* q = offs + (U64)i * 4;
					U32 memOff = (U32)((q[3] << 24) | (q[2] << 16) | (q[1] << 8) | q[0]);
					String nm = names + nameCursor;
					nameCursor += nm.size() + 1;
					if(!ar.index.count(nm))
						ar.index.emplace(std::move(nm), memOff);
				}
			}
			o = dataOff + sz;
			if(o & 1)
				++o;
		}
		return true;
	}
} // namespace rat
