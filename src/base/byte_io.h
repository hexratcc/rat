#ifndef RAT_BYTEIO_H
#define RAT_BYTEIO_H

#include "core.h"

namespace rat {
	namespace le {
		inline U16 rd16(const U8* p) { return (U16)(p[0] | (p[1] << 8)); }
		inline U32 rd32(const U8* p) {
			return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24);
		}
		inline U64 rd64(const U8* p) { return (U64)rd32(p) | ((U64)rd32(p + 4) << 32); }

		inline void put8(List<U8>& b, U8 v) { b.push_back(v); }
		inline void put16(List<U8>& b, U16 v) {
			const U8 tmp[2] = {(U8)v, (U8)(v >> 8)};
			b.insert(b.end(), tmp, tmp + 2);
		}
		inline void put32(List<U8>& b, U32 v) {
			const U8 tmp[4] = {(U8)v, (U8)(v >> 8), (U8)(v >> 16), (U8)(v >> 24)};
			b.insert(b.end(), tmp, tmp + 4);
		}
		inline void put64(List<U8>& b, U64 v) {
			const U8 tmp[8] = {(U8)v,
												 (U8)(v >> 8),
												 (U8)(v >> 16),
												 (U8)(v >> 24),
												 (U8)(v >> 32),
												 (U8)(v >> 40),
												 (U8)(v >> 48),
												 (U8)(v >> 56)};
			b.insert(b.end(), tmp, tmp + 8);
		}

		// store the low len bytes of v
		inline void wr(U8* p, U64 v, U32 len) {
			for(U32 i = 0; i < len; ++i)
				p[i] = (U8)(v >> (i * 8));
		}

		inline void padTo(List<U8>& b, U64 to) {
			if(b.size() < to)
				b.insert(b.end(), to - b.size(), 0);
		}
	} // namespace le
} // namespace rat

#endif
