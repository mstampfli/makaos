#ifndef _MAKAOS_INTTYPES_H
#define _MAKAOS_INTTYPES_H 1

#include <stdint.h>

// Format-string macros for int8/16/32/64 and ptr-sized integers.
// x86_64 ABI: int32_t is "int", int64_t is "long long".
#define PRId8  "d"
#define PRIi8  "i"
#define PRIo8  "o"
#define PRIu8  "u"
#define PRIx8  "x"
#define PRIX8  "X"
#define PRId16 "d"
#define PRIi16 "i"
#define PRIo16 "o"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRIX16 "X"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIo32 "o"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIo64 "llo"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"

#define PRIdMAX  PRId64
#define PRIuMAX  PRIu64
#define PRIxMAX  PRIx64
#define PRIXMAX  PRIX64
#define PRIdPTR  PRId64
#define PRIuPTR  PRIu64
#define PRIxPTR  PRIx64
#define PRIXPTR  PRIX64

#define SCNd32  "d"
#define SCNu32  "u"
#define SCNx32  "x"
#define SCNd64  "lld"
#define SCNu64  "llu"
#define SCNx64  "llx"

intmax_t  strtoimax(const char* s, char** endptr, int base);
uintmax_t strtoumax(const char* s, char** endptr, int base);

#endif
