#ifndef RAT_RATL_LINKERINTERNAL_H
#define RAT_RATL_LINKERINTERNAL_H

#include "elf_read.h"
#include "linker.h"

#include <string_view>

namespace rat {
	namespace detail {
		// an undefined name the shared libs must define
		struct Need {
			std::string_view name; // into the table arena
			B32 required = false;
			B32 found = false;
			U8 type = elf::STT_NOTYPE;
			U64 size = 0;
		};

		struct NeedTable {
			Arena names;
			Map<std::string_view, U32> index;
			List<Need> needs;

			void note(std::string_view name, B32 required);
			B32 matchLibrary(const Lib& lib, U32& foundRequired);
		};

		struct ProgramHeader {
			U32 type, flags;
			U64 offset, vaddr, fileSize, memSize, align;
		};

		// elf_write.cpp
		String joinColon(const List<String>& v);
		void skipLeb(const U8* p, U64& i);
		U32 encSize(U8 enc);
		U8 cieFdeEncoding(const List<U8>& ehFrame, U64 idOff);
		U64 fdeInitialLocation(const List<U8>& ehFrame, U64 pcOff, U8 enc, U64 ehFrameVaddr);
		void putProgramHeader(List<U8>& out, const ProgramHeader& ph);
		void putDynamic(List<U8>& dynamic, I64 tag, U64 value);
		B32 isLinkerSym(const String& n);

		// linker.cpp
		B32 isGotReloc(U32 t);
	} // namespace detail

	struct Linker {
		enum OutSec {
			OInterp,
			OHash,
			ODynsym,
			ODynstr,
			ORelaDyn,
			ORelaPlt,
			OPlt,
			OText,
			ORodata,
			OEhFrame,
			OEhFrameHdr,
			OInitArray,
			OFiniArray,
			OData,
			OTdata,
			OGot,
			OGotPlt,
			ODynamic,
			OBss,
			OTbss,
			kOutSecs
		};

		const LinkOptions& opt;
		String err;

		// after host discovery
		String interp;
		List<String> rpaths;

		List<InObject> objs;
		List<ArchiveFile> archives;
		List<String> libFiles;
		List<String> neededLibs;

		// progbits bucket bytes + sizes for nobits
		List<U8> merged[BucketCount];
		U64 bucketSize[BucketCount] = {0};
		U64 bucketAlign[BucketCount] = {0};

		struct Def {
			U32 obj = 0xffffffffu;
			U32 sym = 0xffffffffu;
			B32 common = false;
			U64 comSize = 0, comAlign = 1;
			U64 addr = 0;
			B32 isFunc = false;
			B32 isTls = false;
			B32 defined = false;
		};
		Map<String, Def> globals;

		Map<String, U32> importIndex;
		List<Import> imports;
		List<String> importNames;

		// got slots keyed by "G:name" / "L:obj:sym"
		Map<String, U32> gotIndex;
		List<GotSlot> gotSlots;

		// synthesized linker/crt sym addrs, filled at end of layout
		Map<String, U64> linkerSyms;

		// comdat group sigs seen, first wins
		Set<String> seenGroups;

		List<U8> ehFrameHdr;

		// merged .debug_* sections, first-seen order
		struct DebugOut {
			String name;
			List<U8> bytes;
			U64 align = 1;
		};
		List<DebugOut> debugOuts;
		Map<String, U32> debugOutIndex;

		U64 vaddr[kOutSecs] = {0};
		U64 foff[kOutSecs] = {0};
		U64 size[kOutSecs] = {0};

		explicit Linker(const LinkOptions& o)
		: opt(o) {}

		B32 run();
		B32 loadInputs();
		B32 pullArchives();
		B32 loadLibraries();
		void collectGlobals();
		B32 resolveExternals();
		B32 importNeeds(const detail::NeedTable& table);
		String gotKey(U32 oi, const InRel& r) const;
		void assignGot();

		List<Pair<U64, U64>> fdeTable() const;
		void buildEhFrameHdr();
		void mergeDebug(const InObject& obj, InSection& s);
		void layout();
		void fillBuckets();
		void sizeSections();
		void place(OutSec s, U64 align, U64& cursor);
		void placeSections();
		void placeImports();
		void placeGlobals();
		void defineLinkerSyms();

		B32 applyRelocs();
		B32 applyReloc(U32 oi, const InRel& r, U64 tlsSize);
		B32 patchStart();
		void applyDebugReloc(const InObject& obj, const InRel& r);
		U64 placedAddr(const InSection& sec, U64 value, B32 isTls) const;
		B32 symbolTarget(const InObject& obj, U32 symIdx, U64& addr, B32& isFunc, B32& isTls);

		B32 write();
		List<U8> dynsymTable(List<U8>& dynstr) const;
		List<U8> hashTable() const;
		List<U8> pltStubs(List<U8>& relaPlt) const;
		List<U8> gotTable(List<U8>& relaDyn) const;
		List<U8> dynamicTable(List<U8>& dynstr, U64 relaDynSize) const;
		detail::ProgramHeader segmentOf(U32 type, U32 flags, OutSec s, U64 align) const;
		void putHeaders(List<U8>& out) const;
		void emitShdrs(List<U8>& out);
		B32 writeFile(const List<U8>& out);
	};
} // namespace rat

#endif
