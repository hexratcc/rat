#include "elf_read.h"

#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace rat {
	using namespace elf;

	namespace detail {
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
	} // namespace detail

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

	B32 loadObject(
			List<U8> img, const String& path, InObject& obj, Set<String>& seenGroups, String& err) {
		List<detail::Shdr> shdrs;
		const U8* shstr = nullptr;
		if(!detail::parseShdrs(img, shdrs, shstr, err))
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
			const detail::Shdr& sh = shdrs[i];
			InSection& s = obj.sections[i];
			s.name = (const char*)(shstr + sh.name);
			s.align = sh.addralign ? sh.addralign : 1;
			s.fileOff = sh.offset;
			s.size = sh.size;
			s.bucket = detail::classify(sh.type, sh.flags);
			if(s.bucket == BRodata && s.name == ".eh_frame")
				s.bucket = BEhFrame;
			s.keep = s.bucket != BNone;
		}

		U32 symSec = 0;
		for(U32 i = 0; i < shdrs.size(); ++i)
			if(shdrs[i].type == SHT_SYMTAB)
				symSec = i;

		// comdat dup: drop only later copies' init/fini members (dup ctors corrupt state)
		{
			for(U32 i = 0; i < shdrs.size(); ++i) {
				const detail::Shdr& sh = shdrs[i];
				if(sh.type != SHT_GROUP)
					continue;
				const U8* g = &im[sh.offset];
				if(!(rd32(g) & GRP_COMDAT))
					continue;
				String sig;
				if(symSec && sh.link == symSec) {
					const detail::Shdr& st = shdrs[sh.link];
					const U8* sp = &im[st.offset + (U64)sh.info * 24];
					const U8* gstr = &im[shdrs[st.link].offset];
					sig = (const char*)(gstr + rd32(sp));
				}
				if(sig.empty() || seenGroups.insert(sig).second)
					continue; // first or unnamed, keep whole
				U32 nmem = (U32)(sh.size / 4);
				for(U32 m = 1; m < nmem; ++m) { // entry 0 is flags word
					U32 sidx = rd32(g + (U64)m * 4);
					if(sidx >= obj.sections.size())
						continue;
					U8 b = obj.sections[sidx].bucket;
					if(b == BInitArray || b == BFiniArray)
						obj.sections[sidx].keep = false;
				}
			}
		}

		// symbols
		if(!symSec) {
			err = "'" + path + "' has no symbol table";
			return false;
		}
		const detail::Shdr& sym = shdrs[symSec];
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
			const detail::Shdr& sh = shdrs[i];
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
				U32 count = (U32)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
				const U8* offs = p + 4;
				const char* names = (const char*)(offs + (U64)count * 4);
				U64 nameCursor = 0;
				for(U32 i = 0; i < count; ++i) {
					const U8* q = offs + (U64)i * 4;
					U32 memOff = (U32)((q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]);
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

	B32 loadLibrary(const String& path, Lib& lib, String& err) {
		std::ifstream f(path, std::ios::binary);
		auto fail = [&](const char* what) {
			err = String(what) + " '" + path + "'";
			return false;
		};
		if(!f)
			return fail("cannot read library");

		U8 eh[64];
		f.read((char*)eh, 64);
		if(!f || eh[0] != 0x7f || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F' || eh[4] != ELFCLASS64 ||
			 eh[5] != ELFDATA2LSB)
			return fail("not a 64-bit LE ELF library");
		U64 shoff = rd64(eh + 40);
		if(rd16(eh + 58) != 64)
			return fail("bad section header size in");
		U32 shnum = rd16(eh + 60);

		List<U8> sh(shnum * 64);
		f.seekg((std::streamoff)shoff);
		f.read((char*)sh.data(), (std::streamsize)sh.size());
		if(!f)
			return fail("truncated section headers in");

		U64 symOff = 0, symSize = 0, strOff = 0, strSize = 0;
		for(U32 i = 0; i < shnum; ++i) {
			const U8* p = &sh[(U64)i * 64];
			if(rd32(p + 4) != SHT_DYNSYM)
				continue;
			symOff = rd64(p + 24);
			symSize = rd64(p + 32);
			U32 link = rd32(p + 40);
			if(link < shnum) {
				const U8* d = &sh[(U64)link * 64];
				strOff = rd64(d + 24);
				strSize = rd64(d + 32);
			}
			break;
		}
		if(!symSize || !strSize)
			return fail("no dynamic symbol table in");

		lib.dynsym.resize(symSize);
		f.seekg((std::streamoff)symOff);
		f.read((char*)lib.dynsym.data(), (std::streamsize)symSize);
		lib.dynstr.resize(strSize);
		f.seekg((std::streamoff)strOff);
		f.read((char*)lib.dynstr.data(), (std::streamsize)strSize);
		if(!f)
			return fail("truncated dynamic tables in");

		lib.soname = path.substr(path.find_last_of('/') + 1);
		return true;
	}

	B32 findLibrary(const String& l, const List<String>& paths, String& found) {
		const char* suffixes[] = {".so.6", ".so.1", ".so"};
		for(const String& dir : paths) {
			String base = dir;
			if(!base.empty() && base.back() != '/')
				base += '/';
			for(const char* suf : suffixes) {
				String cand = base + "lib" + l + suf;
				struct stat st;
				if(stat(cand.c_str(), &st) == 0) {
					found = cand;
					return true;
				}
			}
		}
		return false;
	}

	// probe host loader/libdirs from /proc/self (fhs-independent); only when opt empty

	// loader from ratl's own PT_INTERP, empty on failure
	const String& hostLoader() {
		static const String p = [] {
			std::ifstream f("/proc/self/exe", std::ios::binary);
			if(!f)
				return String();
			U8 eh[64];
			if(!f.read((char*)eh, 64))
				return String();
			U64 phoff = rd64(eh + 32);
			U16 phentsize = rd16(eh + 54);
			U16 phnum = rd16(eh + 56);
			for(U16 i = 0; i < phnum; ++i) {
				U8 ph[56];
				f.seekg((std::streamoff)(phoff + (U64)i * phentsize));
				if(!f.read((char*)ph, 56))
					break;
				if(rd32(ph) != PT_INTERP)
					continue;
				U64 off = rd64(ph + 8), sz = rd64(ph + 32);
				if(!sz || sz > 4096)
					break;
				String s(sz, '\0');
				f.seekg((std::streamoff)off);
				if(!f.read(&s[0], (std::streamsize)sz))
					break;
				if(!s.empty() && s.back() == '\0')
					s.pop_back();
				return s;
			}
			return String();
		}();
		return p;
	}

	// dirs of libs ratl has mapped, first-seen order
	const List<String>& hostLibDirs() {
		static const List<String> dirs = [] {
			List<String> out;
			Set<String> seen;
			std::ifstream f("/proc/self/maps");
			String line;
			while(std::getline(f, line)) {
				auto slash = line.find('/');
				if(slash == String::npos)
					continue;
				String path = line.substr(slash);
				auto base = path.find_last_of('/');
				if(base == String::npos || path.find(".so", base) == String::npos)
					continue;
				String dir = path.substr(0, base);
				if(seen.insert(dir).second)
					out.push_back(dir);
			}
			return out;
		}();
		return dirs;
	}
} // namespace rat
