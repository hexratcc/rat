#ifndef RAT_RATL_INFLATE_H
#define RAT_RATL_INFLATE_H

#include "core.h"

namespace rat {
	namespace detail {
		struct BitReader {
			const U8* data = nullptr;
			U64 size = 0;
			U64 pos = 0; // next unread byte
			U64 bitBuf = 0;
			U32 bitCount = 0;
		};

		// canonical code, symbols sorted by code length
		struct Huffman {
			U16 counts[16];
			U16 symbols[288];
		};

		U32 readBits(BitReader& br, U32 n);
		B32 huffBuild(Huffman& h, const U8* lengths, U32 n);
		I32 huffDecode(BitReader& br, const Huffman& h);
		B32
		inflateCodes(BitReader& br, const Huffman& lit, const Huffman& dist, List<U8>& out, U64 limit);
		B32 inflateStored(BitReader& br, List<U8>& out, U64 limit);
		B32 readCodeLengths(BitReader& br, U8* lengths, U32& nlen, U32& ndist);
		B32 inflateHuffman(BitReader& br, B32 dynamic, List<U8>& out, U64 limit);
		U32 adler32(const List<U8>& bytes);
	} // namespace detail

	// zlib stream (rfc 1950/1951) into out, fails on bad data, past limit bytes or bad adler32
	B32 zlibInflate(const U8* src, U64 size, List<U8>& out, U64 limit);
} // namespace rat

#endif
