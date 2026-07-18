#include "Target/ObjectFile.h"

#include "Target/X86Coff.h"
#include "Target/X86Elf.h"

namespace rat {
	void ObjectFile::put8(List<U8>& b, U8 v) { b.push_back(v); }
	void ObjectFile::put16(List<U8>& b, U16 v) {
		b.push_back((U8)(v));
		b.push_back((U8)(v >> 8));
	}
	void ObjectFile::put32(List<U8>& b, U32 v) {
		for(U32 i = 0; i < 4; ++i)
			b.push_back((U8)(v >> (i * 8)));
	}
	void ObjectFile::put64(List<U8>& b, U64 v) {
		for(U32 i = 0; i < 8; ++i)
			b.push_back((U8)(v >> (i * 8)));
	}

	ObjectFile::ObjectFile() { syms.push_back({"", Text, 0, true, false, false}); }

	void ObjectFile::padTo(List<U8>& b, U64 target) {
		if(b.size() < target)
			b.insert(b.end(), target - b.size(), 0);
	}

	List<U8>& ObjectFile::bytesOf(Section sec) {
		assert(sec != Bss && "bss carries no bytes");
		return raw[(U32)sec];
	}

	const List<U8>& ObjectFile::bytesOf(Section sec) const {
		assert(sec != Bss && "bss carries no bytes");
		return raw[(U32)sec];
	}

	U32 ObjectFile::sectionSize(Section sec) const {
		return sec == Bss ? bssSize : (U32)raw[(U32)sec].size();
	}

	U32 ObjectFile::append(Section sec, const U8* bytes, U32 len) {
		if(sec == Bss)
			return appendZero(sec, len);
		List<U8>& b = bytesOf(sec);
		U32 off = (U32)b.size();
		b.insert(b.end(), bytes, bytes + len);
		return off;
	}

	U32 ObjectFile::appendZero(Section sec, U32 len) {
		if(sec == Bss) {
			U32 off = bssSize;
			bssSize += len;
			return off;
		}
		List<U8>& b = bytesOf(sec);
		U32 off = (U32)b.size();
		b.insert(b.end(), len, 0);
		return off;
	}

	U32 ObjectFile::align(Section sec, U32 a) {
		U32 sz = sectionSize(sec);
		U32 pad = (a - (sz % a)) % a;
		if(pad)
			appendZero(sec, pad);
		return sectionSize(sec);
	}

	U32 ObjectFile::symbolIndex(const String& name) {
		auto it = symByName.find(name);
		if(it != symByName.end())
			return it->second;
		U32 idx = (U32)syms.size();
		syms.push_back({name, Text, 0, false, true, false});
		symByName[name] = idx;
		return idx;
	}

	void
	ObjectFile::defineSymbol(const String& name, Section sec, U32 offset, B32 global, B32 isFunc) {
		auto it = symByName.find(name);
		if(it != symByName.end()) {
			Sym& s = syms[it->second];
			s.sec = sec;
			s.offset = offset;
			s.defined = true;
			s.global = global;
			s.isFunc = isFunc;
			return;
		}
		U32 idx = (U32)syms.size();
		syms.push_back({name, sec, offset, true, global, isFunc});
		symByName[name] = idx;
	}

	void
	ObjectFile::addReloc(Section sec, U32 offset, const String& symbol, RelocKind kind, I64 addend) {
		relocs.push_back({sec, offset, symbolIndex(symbol), kind, addend});
	}

	UniquePtr<ObjectFile> createObjectFile(OS os) {
		if(os == OS::Windows)
			return std::make_unique<CoffObject>();
		return std::make_unique<ElfObject>();
	}
} // namespace rat
