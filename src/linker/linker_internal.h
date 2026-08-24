#ifndef RAT_RATL_LINKERINTERNAL_H
#define RAT_RATL_LINKERINTERNAL_H

#include "elf_read.h"
#include "linker.h"

namespace rat {
	namespace detail {
		// elf_write.cpp
		String joinColon(const List<String>& v);
		U64 readUleb(const U8* p, U64& i);
		I64 readSleb(const U8* p, U64& i);
		U32 encSize(U8 enc);

		// linker.cpp
		B32 isLinkerSym(const String& n);
	} // namespace detail

	struct Linker {
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

		// _start
		List<U8> startCode;
		U64 startLeaDisp = 0;
		U64 startCallDisp = 0;

		// .eh_frame_hdr
		List<U8> ehFrameHdr;
		U32 ehFdeCount = 0;

		U64 vaddr[24] = {0};
		U64 foff[24] = {0};
		U64 size[24] = {0};
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
		U64 bucketVaddr[BucketCount] = {0};

		explicit Linker(const LinkOptions& o)
		: opt(o) {}

		B32 run();
		B32 loadInputs();
		B32 pullArchives();
		B32 loadLibraries();
		void collectGlobals();
		B32 resolveExternals();
		Import& intern(const String& name, Import::Kind kind);
		void assignGot();
		void buildStart();
		U32 countFdes();
		void buildEhFrameHdr();
		void layout();
		B32 applyRelocs();
		B32 symbolTarget(const InObject& obj, U32 symIdx, U64& addr, B32& isFunc, B32& isTls);
		B32 write();

		static OutSec outSecOf(U8 bucket) {
			switch(bucket) {
			case BText:
				return OText;
			case BRodata:
				return ORodata;
			case BEhFrame:
				return OEhFrame;
			case BInitArray:
				return OInitArray;
			case BFiniArray:
				return OFiniArray;
			case BData:
				return OData;
			case BBss:
				return OBss;
			case BTdata:
				return OTdata;
			case BTbss:
				return OTbss;
			}
			return OText;
		}
	};
} // namespace rat

#endif
