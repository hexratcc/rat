#ifndef RAT_RATL_ELFFILE_H
#define RAT_RATL_ELFFILE_H

#include "byte_io.h"
#include "core.h"

// elf64 le constants + helpers
namespace rat {
	namespace elf {
		// e_type
		constexpr U16 ET_REL = 1;
		constexpr U16 ET_EXEC = 2;
		constexpr U16 ET_DYN = 3;
		constexpr U16 EM_X86_64 = 62;
		constexpr U8 ELFCLASS64 = 2;
		constexpr U8 ELFDATA2LSB = 1;
		constexpr U8 EV_CURRENT = 1;

		// section types
		constexpr U32 SHT_NULL = 0;
		constexpr U32 SHT_PROGBITS = 1;
		constexpr U32 SHT_SYMTAB = 2;
		constexpr U32 SHT_STRTAB = 3;
		constexpr U32 SHT_RELA = 4;
		constexpr U32 SHT_NOBITS = 8;
		constexpr U32 SHT_DYNSYM = 11;
		constexpr U32 SHT_INIT_ARRAY = 14;
		constexpr U32 SHT_FINI_ARRAY = 15;
		constexpr U32 SHT_GROUP = 17;

		// section flags
		constexpr U64 SHF_WRITE = 0x1;
		constexpr U64 SHF_ALLOC = 0x2;
		constexpr U64 SHF_EXECINSTR = 0x4;
		constexpr U64 SHF_INFO_LINK = 0x40;
		constexpr U64 SHF_TLS = 0x400;

		// group flags
		constexpr U32 GRP_COMDAT = 0x1;

		// symbol bind / type / visibility
		constexpr U8 STB_LOCAL = 0;
		constexpr U8 STB_GLOBAL = 1;
		constexpr U8 STB_WEAK = 2;
		constexpr U8 STB_GNU_UNIQUE = 10; // global def
		constexpr U8 STT_NOTYPE = 0;
		constexpr U8 STT_OBJECT = 1; // data import -> COPY; else PLT
		constexpr U8 STT_FUNC = 2;
		constexpr U8 STT_TLS = 6;
		constexpr U8 STT_GNU_IFUNC = 10;
		constexpr U16 SHN_UNDEF = 0;
		constexpr U16 SHN_ABS = 0xfff1;
		constexpr U16 SHN_COMMON = 0xfff2;

		// program header
		constexpr U32 PT_LOAD = 1;
		constexpr U32 PT_DYNAMIC = 2;
		constexpr U32 PT_INTERP = 3;
		constexpr U32 PT_NOTE = 4;
		constexpr U32 PT_TLS = 7;
		constexpr U32 PT_PHDR = 6;
		constexpr U32 PT_GNU_EH_FRAME = 0x6474e550;
		constexpr U32 PT_GNU_STACK = 0x6474e551;
		constexpr U32 PF_X = 1;
		constexpr U32 PF_W = 2;
		constexpr U32 PF_R = 4;

		// dynamic tags
		constexpr I64 DT_NULL = 0;
		constexpr I64 DT_NEEDED = 1;
		constexpr I64 DT_PLTRELSZ = 2;
		constexpr I64 DT_PLTGOT = 3;
		constexpr I64 DT_HASH = 4;
		constexpr I64 DT_STRTAB = 5;
		constexpr I64 DT_SYMTAB = 6;
		constexpr I64 DT_RELA = 7;
		constexpr I64 DT_RELASZ = 8;
		constexpr I64 DT_RELAENT = 9;
		constexpr I64 DT_STRSZ = 10;
		constexpr I64 DT_SYMENT = 11;
		constexpr I64 DT_PLTREL = 20;
		constexpr I64 DT_JMPREL = 23;
		constexpr I64 DT_INIT_ARRAY = 25;
		constexpr I64 DT_FINI_ARRAY = 26;
		constexpr I64 DT_INIT_ARRAYSZ = 27;
		constexpr I64 DT_FINI_ARRAYSZ = 28;
		constexpr I64 DT_RUNPATH = 29;
		constexpr I64 DT_FLAGS = 30;
		constexpr I64 DT_FLAGS_1 = 0x6ffffffb;
		constexpr U64 DF_BIND_NOW = 0x8;
		constexpr U64 DF_1_NOW = 0x1;

		// x86-64 relocation types
		constexpr U32 R_X86_64_64 = 1;
		constexpr U32 R_X86_64_PC32 = 2;
		constexpr U32 R_X86_64_GOT32 = 3;
		constexpr U32 R_X86_64_PLT32 = 4;
		constexpr U32 R_X86_64_COPY = 5;
		constexpr U32 R_X86_64_GLOB_DAT = 6;
		constexpr U32 R_X86_64_JUMP_SLOT = 7;
		constexpr U32 R_X86_64_GOTPCREL = 9;
		constexpr U32 R_X86_64_32 = 10;
		constexpr U32 R_X86_64_32S = 11;
		constexpr U32 R_X86_64_16 = 12;
		constexpr U32 R_X86_64_GOTTPOFF = 22;
		constexpr U32 R_X86_64_TPOFF32 = 23;
		constexpr U32 R_X86_64_PC64 = 24;
		constexpr U32 R_X86_64_SIZE32 = 32;
		constexpr U32 R_X86_64_SIZE64 = 33;
		constexpr U32 R_X86_64_GOTPCRELX = 41;
		constexpr U32 R_X86_64_REX_GOTPCRELX = 42;

		// forwarders to base/ByteIO.h, keep call sites unchanged
		inline U16 rd16(const U8* p) { return le::rd16(p); }
		inline U32 rd32(const U8* p) { return le::rd32(p); }
		inline U64 rd64(const U8* p) { return le::rd64(p); }
		inline void wr16(List<U8>& b, U16 v) { le::put16(b, v); }
		inline void wr32(List<U8>& b, U32 v) { le::put32(b, v); }
		inline void wr64(List<U8>& b, U64 v) { le::put64(b, v); }
		inline void wrPad(List<U8>& b, U64 to) { le::padTo(b, to); }

		// sysv symbol hash
		inline U32 sysvHash(const C8* name) {
			U32 h = 0, g;
			for(const U8* s = (const U8*)name; *s; ++s) {
				h = (h << 4) + *s;
				if((g = h & 0xf0000000))
					h ^= g >> 24;
				h &= ~g;
			}
			return h;
		}
	} // namespace elf
} // namespace rat

#endif
