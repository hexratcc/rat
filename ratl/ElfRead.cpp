#include "ElfRead.h"

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
			U16 shnum = rd16(&img[62]);
			U16 shstrndx = rd16(&img[60]);
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
} // namespace rat
