#include "inflate.h"

#include "byte_io.h"

namespace rat {
	namespace detail {
		constexpr U16 kLenBase[29] = {3,	4,	5,	6,	7,	8,	9,	10, 11,	 13,	15,	 17,	19,	 23, 27,
																	31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
		constexpr U8 kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
																	2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
		constexpr U16 kDistBase[30] = {1,		 2,		 3,		 4,		 5,		 7,		 9,		 13,		17,		 25,
																	 33,	 49,	 65,	 97,	 129,	 193,	 257,	 385,		513,	 769,
																	 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
		constexpr U8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,	3,	3,	4,	4,	5,	5,	6,
																	 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
		constexpr U8 kRepeatBase[3] = {3, 3, 11}; // code length symbols 16 17 18
		constexpr U8 kRepeatBits[3] = {2, 3, 7};
		constexpr U8 kCodeLenOrder[19] = {
				16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

		// lsb-first, n <= 16
		U32 readBits(BitReader& br, U32 n) {
			while(br.bitCount < n) {
				if(br.pos >= br.size)
					return 0; // past the end reads zeros, the limit or adler32 rejects it
				br.bitBuf |= (U64)br.data[br.pos++] << br.bitCount;
				br.bitCount += 8;
			}
			U32 v = (U32)(br.bitBuf & ((1ull << n) - 1));
			br.bitBuf >>= n;
			br.bitCount -= n;
			return v;
		}

		B32 huffBuild(Huffman& h, const U8* lengths, U32 n) {
			memset(h.counts, 0, sizeof(h.counts));
			for(U32 i = 0; i < n; ++i)
				h.counts[lengths[i]]++;
			I32 left = 1;
			for(U32 len = 1; len < 16; ++len) {
				left = (left << 1) - h.counts[len];
				if(left < 0)
					return false; // over-subscribed
			}
			U16 offs[16] = {};
			for(U32 len = 1; len < 15; ++len)
				offs[len + 1] = (U16)(offs[len] + h.counts[len]);
			for(U32 sym = 0; sym < n; ++sym)
				if(lengths[sym])
					h.symbols[offs[lengths[sym]]++] = (U16)sym;
			return true;
		}

		// -1 on bad code
		I32 huffDecode(BitReader& br, const Huffman& h) {
			I32 code = 0, first = 0, index = 0;
			for(U32 len = 1; len < 16; ++len) {
				code |= (I32)readBits(br, 1);
				I32 count = h.counts[len];
				if(code - first < count)
					return h.symbols[index + code - first];
				index += count;
				first = (first + count) << 1;
				code <<= 1;
			}
			return -1;
		}

		B32
		inflateCodes(BitReader& br, const Huffman& lit, const Huffman& dist, List<U8>& out, U64 limit) {
			for(;;) {
				I32 sym = huffDecode(br, lit);
				if(sym < 0 || out.size() > limit)
					return false;
				if(sym == 256)
					return true;
				if(sym < 256) {
					out.push_back((U8)sym);
					continue;
				}
				sym -= 257;
				if(sym >= 29)
					return false;
				U64 len = kLenBase[sym] + readBits(br, kLenExtra[sym]);
				I32 dsym = huffDecode(br, dist);
				if(dsym < 0 || dsym >= 30)
					return false;
				U64 back = kDistBase[dsym] + readBits(br, kDistExtra[dsym]);
				if(back > out.size())
					return false;
				for(U64 i = 0; i < len; ++i) {
					U8 b = out[out.size() - back];
					out.push_back(b);
				}
			}
		}

		B32 inflateStored(BitReader& br, List<U8>& out, U64 limit) {
			br.bitBuf = 0; // drop to byte boundary, bitCount < 8 here
			br.bitCount = 0;
			if(br.pos + 4 > br.size)
				return false;
			U32 len = le::rd16(br.data + br.pos);
			U32 nlen = le::rd16(br.data + br.pos + 2);
			br.pos += 4;
			if(len != (~nlen & 0xffff) || br.pos + len > br.size || out.size() + len > limit)
				return false;
			out.insert(out.end(), br.data + br.pos, br.data + br.pos + len);
			br.pos += len;
			return true;
		}

		B32 readCodeLengths(BitReader& br, U8* lengths, U32& nlen, U32& ndist) {
			nlen = readBits(br, 5) + 257;
			ndist = readBits(br, 5) + 1;
			U32 ncode = readBits(br, 4) + 4;
			if(nlen > 286 || ndist > 30)
				return false;
			for(U32 i = 0; i < ncode; ++i)
				lengths[kCodeLenOrder[i]] = (U8)readBits(br, 3);
			Huffman lencode;
			if(!huffBuild(lencode, lengths, 19))
				return false;
			U32 index = 0;
			while(index < nlen + ndist) {
				I32 sym = huffDecode(br, lencode);
				if(sym < 0)
					return false;
				if(sym < 16) {
					lengths[index++] = (U8)sym;
					continue;
				}
				if(sym == 16 && index == 0)
					return false;
				U8 rep = sym == 16 ? lengths[index - 1] : 0;
				U32 count = kRepeatBase[sym - 16] + readBits(br, kRepeatBits[sym - 16]);
				if(index + count > nlen + ndist)
					return false;
				memset(lengths + index, rep, count);
				index += count;
			}
			return lengths[256] != 0; // has an end-of-block code
		}

		// fixed or dynamic huffman block
		B32 inflateHuffman(BitReader& br, B32 dynamic, List<U8>& out, U64 limit) {
			U8 lengths[288 + 30] = {};
			U32 nlen = 288, ndist = 30;
			if(!dynamic) {
				memset(lengths, 8, 144);
				memset(lengths + 144, 9, 112);
				memset(lengths + 256, 7, 24);
				memset(lengths + 280, 8, 8);
				memset(lengths + 288, 5, 30);
			} else if(!readCodeLengths(br, lengths, nlen, ndist))
				return false;
			Huffman lit, dist;
			if(!huffBuild(lit, lengths, nlen) || !huffBuild(dist, lengths + nlen, ndist))
				return false;
			return inflateCodes(br, lit, dist, out, limit);
		}

		U32 adler32(const List<U8>& bytes) {
			U32 a = 1, b = 0;
			for(U8 byte : bytes) {
				a = (a + byte) % 65521;
				b = (b + a) % 65521;
			}
			return (b << 16) | a;
		}
	} // namespace detail

	B32 zlibInflate(const U8* src, U64 size, List<U8>& out, U64 limit) {
		if(size < 6 || (src[0] & 0xf) != 8 || (src[0] >> 4) > 7 || (src[1] & 0x20) ||
			 ((U32)src[0] << 8 | src[1]) % 31)
			return false;
		detail::BitReader br;
		br.data = src;
		br.size = size;
		br.pos = 2;
		out.clear();
		limit = std::min<U64>(limit, size * 1032); // deflate expands at most ~1032x
		out.reserve(limit);
		U32 final = 0;
		while(!final) {
			final = detail::readBits(br, 1);
			U32 type = detail::readBits(br, 2);
			B32 ok = false;
			if(type == 0)
				ok = detail::inflateStored(br, out, limit);
			else if(type < 3)
				ok = detail::inflateHuffman(br, type == 2, out, limit);
			if(!ok || out.size() > limit)
				return false;
		}
		if(br.pos + 4 > br.size)
			return false;
		const U8* t = br.data + br.pos;
		U32 want = (U32)t[0] << 24 | (U32)t[1] << 16 | (U32)t[2] << 8 | t[3];
		return detail::adler32(out) == want;
	}
} // namespace rat
