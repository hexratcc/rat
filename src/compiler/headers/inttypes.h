#ifndef _RATCC_INTTYPES_H
#define _RATCC_INTTYPES_H

#include <stdint.h>

#if defined(_WIN32)
#define __RAT_PRI64 "ll"
#else
#define __RAT_PRI64 "l"
#endif

#define PRId8 "d"
#define PRIi8 "i"
#define PRIu8 "u"
#define PRIo8 "o"
#define PRIx8 "x"
#define PRIX8 "X"
#define PRId16 "d"
#define PRIi16 "i"
#define PRIu16 "u"
#define PRIo16 "o"
#define PRIx16 "x"
#define PRIX16 "X"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIo32 "o"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 __RAT_PRI64 "d"
#define PRIi64 __RAT_PRI64 "i"
#define PRIu64 __RAT_PRI64 "u"
#define PRIo64 __RAT_PRI64 "o"
#define PRIx64 __RAT_PRI64 "x"
#define PRIX64 __RAT_PRI64 "X"
#define PRIdMAX PRId64
#define PRIiMAX PRIi64
#define PRIuMAX PRIu64
#define PRIoMAX PRIo64
#define PRIxMAX PRIx64
#define PRIXMAX PRIX64
#define PRIdPTR PRId64
#define PRIiPTR PRIi64
#define PRIuPTR PRIu64
#define PRIoPTR PRIo64
#define PRIxPTR PRIx64
#define PRIXPTR PRIX64

#define SCNd8 "hhd"
#define SCNi8 "hhi"
#define SCNu8 "hhu"
#define SCNx8 "hhx"
#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNu16 "hu"
#define SCNx16 "hx"
#define SCNd32 "d"
#define SCNi32 "i"
#define SCNu32 "u"
#define SCNx32 "x"
#define SCNd64 __RAT_PRI64 "d"
#define SCNi64 __RAT_PRI64 "i"
#define SCNu64 __RAT_PRI64 "u"
#define SCNx64 __RAT_PRI64 "x"

typedef struct {
	intmax_t quot;
	intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t n);
intmax_t strtoimax(const char* s, char** end, int base);
uintmax_t strtoumax(const char* s, char** end, int base);

#endif
