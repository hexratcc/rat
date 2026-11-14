#include "elf_read.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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

		B32 parseShdrs(const List<U8>& img, List<SectionHeader>& out, const U8*& shstr, String& err) {
			if(img.size() < 64 || !hasElfMagic(img.data())) {
				err = "not an ELF file";
				return false;
			}
			if(img[4] != ELFCLASS64 || img[5] != ELFDATA2LSB) {
				err = "not a 64-bit little-endian ELF";
				return false;
			}
			U64 shoff = le::rd64(&img[40]);
			U16 shentsize = le::rd16(&img[58]);
			U16 shnum = le::rd16(&img[60]);
			U16 shstrndx = le::rd16(&img[62]);
			if(shentsize != 64 || shoff + (U64)shnum * 64 > img.size()) {
				err = "malformed section header table";
				return false;
			}
			out.clear();
			for(U16 i = 0; i < shnum; ++i) {
				const U8* p = &img[shoff + (U64)i * 64];
				SectionHeader s;
				s.name = le::rd32(p);
				s.type = le::rd32(p + 4);
				s.flags = le::rd64(p + 8);
				s.offset = le::rd64(p + 24);
				s.size = le::rd64(p + 32);
				s.link = le::rd32(p + 40);
				s.info = le::rd32(p + 44);
				s.align = le::rd64(p + 48);
				s.entSize = le::rd64(p + 56);
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
			f.read((C8*)out.data(), n);
		return (B32)f.good() || f.eof();
	}

	namespace detail {
		// n bytes at offset of f, false when the file is shorter
		B32 readAt(std::ifstream& f, U64 offset, U64 n, List<U8>& out) {
			f.seekg(0, std::ios::end);
			U64 fileSize = (U64)f.tellg();
			if(!f || n > fileSize || offset > fileSize - n)
				return false;
			out.resize(n);
			f.seekg((std::streamoff)offset);
			f.read((C8*)out.data(), (std::streamsize)n);
			return !f.fail();
		}
	} // namespace detail

	B32 loadObject(
			List<U8> img, const String& path, InObject& obj, Set<String>& seenGroups, String& err) {
		List<SectionHeader> shdrs;
		const U8* shstr = nullptr;
		if(!detail::parseShdrs(img, shdrs, shstr, err))
			return false;
		if(le::rd16(&img[16]) != ET_REL) {
			err = "'" + path + "' is not a relocatable object";
			return false;
		}
		obj.path = path;
		obj.image = std::move(img);
		const List<U8>& im = obj.image;

		// one InSection per index, syms and relocs reference original indices
		obj.sections.resize(shdrs.size());
		B32 debugCompressed = false;
		U32 symSec = 0;
		for(U32 i = 0; i < shdrs.size(); ++i) {
			const SectionHeader& sh = shdrs[i];
			InSection& s = obj.sections[i];
			s.name = (const C8*)(shstr + sh.name);
			s.align = sh.align ? sh.align : 1;
			s.fileOff = sh.offset;
			s.size = sh.size;
			s.bucket = detail::classify(sh.type, sh.flags);
			if(s.bucket == BRodata && s.name == ".eh_frame")
				s.bucket = BEhFrame;
			if(s.bucket == BNone && sh.type == SHT_PROGBITS && s.name.rfind(".debug_", 0) == 0)
				s.bucket = BDebug;
			s.keep = s.bucket != BNone;
			debugCompressed |= s.bucket == BDebug && (sh.flags & SHF_COMPRESSED);
			if(sh.type == SHT_SYMTAB)
				symSec = i;
		}
		// compressed debug sections are not read, drop all of the object's debug info
		if(debugCompressed) {
			std::cerr << "ratl: warning: '" << path
								<< "': compressed debug sections, debug info dropped\n";
			for(InSection& s : obj.sections)
				if(s.bucket == BDebug)
					s.keep = false;
		}
		if(!symSec) {
			err = "'" + path + "' has no symbol table";
			return false;
		}
		const SectionHeader& sym = shdrs[symSec];
		const U8* strtab = &im[shdrs[sym.link].offset];

		// comdat dup, drop only later copies' init and fini members (dup ctors corrupt state)
		for(const SectionHeader& sh : shdrs) {
			B32 comdat = sh.type == SHT_GROUP && (le::rd32(&im[sh.offset]) & GRP_COMDAT);
			if(!comdat || sh.link != symSec)
				continue;
			const U8* sigSym = &im[sym.offset + (U64)sh.info * 24];
			String sig = (const C8*)(strtab + le::rd32(sigSym));
			if(sig.empty() || seenGroups.insert(sig).second)
				continue;														 // first or unnamed, keep whole
			for(U64 m = 1; m < sh.size / 4; ++m) { // entry 0 is flags word
				U32 sidx = le::rd32(&im[sh.offset + m * 4]);
				if(sidx >= obj.sections.size())
					continue;
				InSection& member = obj.sections[sidx];
				if(member.bucket == BInitArray || member.bucket == BFiniArray)
					member.keep = false;
			}
		}

		obj.syms.reserve(sym.size / 24);
		for(U64 at = sym.offset; at + 24 <= sym.offset + sym.size; at += 24) {
			const U8* p = &im[at];
			InSym s;
			s.name = (const C8*)(strtab + le::rd32(p));
			s.bind = (U8)(p[4] >> 4);
			s.type = (U8)(p[4] & 0xf);
			s.shndx = le::rd16(p + 6);
			s.value = le::rd64(p + 8);
			s.size = le::rd64(p + 16); // for commons value is the align
			s.abs = s.shndx == SHN_ABS;
			s.common = s.shndx == SHN_COMMON;
			B32 inKeptSection = s.shndx < obj.sections.size() && obj.sections[s.shndx].keep;
			s.undef = !s.abs && !s.common && !inKeptSection; // a def in a dropped section too
			obj.syms.push_back(std::move(s));
		}

		// relocs, keep those patching a kept section
		for(const SectionHeader& sh : shdrs) {
			if(sh.type != SHT_RELA || sh.info >= obj.sections.size() || !obj.sections[sh.info].keep)
				continue;
			for(U64 at = sh.offset; at + 24 <= sh.offset + sh.size; at += 24) {
				U64 offset = le::rd64(&im[at]);
				U64 info = le::rd64(&im[at + 8]);
				I64 addend = (I64)le::rd64(&im[at + 16]);
				obj.rels.push_back({sh.info, offset, (U32)(info >> 32), (U32)info, addend});
			}
		}
		return true;
	}

	U64 arMemberSize(const List<U8>& d, U64 hdrOff) {
		String s((const C8*)&d[hdrOff + 48], 10);
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
			C8 nm0 = (C8)d[o], nm1 = (C8)d[o + 1];
			U64 sz = arMemberSize(d, o);
			U64 dataOff = o + 60;
			if(nm0 == '/' && nm1 == ' ') {
				// sysv armap: BE count, BE offsets, NUL names
				const U8* p = &d[dataOff];
				U32 count = (U32)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
				const U8* offs = p + 4;
				const C8* names = (const C8*)(offs + (U64)count * 4);
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
			o = alignUp(dataOff + sz, 2);
		}
		return true;
	}

	B32 loadLibrary(const String& path, Lib& lib, String& err) {
		std::ifstream f(path, std::ios::binary);
		List<U8> header, table;
		B32 isElf = detail::readAt(f, 0, 64, header) && hasElfMagic(header.data());
		B32 is64Le = isElf && header[4] == ELFCLASS64 && header[5] == ELFDATA2LSB;
		if(!is64Le || le::rd16(&header[58]) != 64) {
			err = "not a 64-bit little-endian ELF library '" + path + "'";
			return false;
		}
		U64 shnum = le::rd16(&header[60]);
		U64 shoff = le::rd64(&header[40]);
		if(!detail::readAt(f, shoff, shnum * 64, table)) {
			err = "truncated section headers in '" + path + "'";
			return false;
		}
		for(U64 i = 0; i < shnum; ++i) {
			const U8* sym = &table[i * 64];
			U32 link = le::rd32(sym + 40);
			if(le::rd32(sym + 4) != SHT_DYNSYM || link >= shnum)
				continue;
			const U8* str = &table[(U64)link * 64];
			B32 dynsymRead = detail::readAt(f, le::rd64(sym + 24), le::rd64(sym + 32), lib.dynsym);
			if(!dynsymRead || !detail::readAt(f, le::rd64(str + 24), le::rd64(str + 32), lib.dynstr))
				break;
			lib.soname = path.substr(path.find_last_of('/') + 1);
			return true;
		}
		err = "no dynamic symbol table in '" + path + "'";
		return false;
	}

	B32 findLibrary(const String& l, const List<String>& paths, String& found) {
		const C8* suffixes[] = {".so.6", ".so.1", ".so"};
		for(const String& dir : paths) {
			String base = dir;
			if(!base.empty() && base.back() != '/')
				base += '/';
			for(const C8* suf : suffixes) {
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

	namespace detail {
		// loader from ratl's own PT_INTERP, empty on failure
		String probeHostLoader() {
			std::ifstream f("/proc/self/exe", std::ios::binary);
			List<U8> header, phdrs, interp;
			if(!readAt(f, 0, 64, header))
				return String();
			U64 phoff = le::rd64(&header[32]);
			U16 phentsize = le::rd16(&header[54]);
			U16 phnum = le::rd16(&header[56]);
			if(phentsize < 56 || !readAt(f, phoff, (U64)phnum * phentsize, phdrs))
				return String();
			for(U16 i = 0; i < phnum; ++i) {
				const U8* ph = &phdrs[(U64)i * phentsize];
				if(le::rd32(ph) != PT_INTERP || !readAt(f, le::rd64(ph + 8), le::rd64(ph + 32), interp))
					continue;
				const C8* path = (const C8*)interp.data();
				return String(path, strnlen(path, interp.size())); // up to the nul
			}
			return String();
		}

		// dirs of libs ratl has mapped, first-seen order
		List<String> probeHostLibDirs() {
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
		}
	} // namespace detail

	// probed from /proc/self, fhs independent
	const String& hostLoader() {
		static const String loader = detail::probeHostLoader();
		return loader;
	}

	const List<String>& hostLibDirs() {
		static const List<String> dirs = detail::probeHostLibDirs();
		return dirs;
	}
} // namespace rat
