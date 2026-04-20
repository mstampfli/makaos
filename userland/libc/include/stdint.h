#ifndef _MAKAOS_STDINT_H
#define _MAKAOS_STDINT_H 1
// ISO C stdint.h — exact-width integer types and their limit macros.

typedef signed   char      int8_t;
typedef unsigned char      uint8_t;
typedef signed   short     int16_t;
typedef unsigned short     uint16_t;
typedef signed   int       int32_t;
typedef unsigned int       uint32_t;
typedef signed   long long int64_t;
typedef unsigned long long uint64_t;

typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;
typedef int64_t            intptr_t;
typedef uint64_t           uintptr_t;

#define INT8_MIN    (-128)
#define INT8_MAX    127
#define UINT8_MAX   255u
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define UINT16_MAX  65535u
#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define UINT32_MAX  4294967295u
#define INT64_MIN   (-9223372036854775807ll - 1)
#define INT64_MAX   9223372036854775807ll
#define UINT64_MAX  18446744073709551615ull
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define SIZE_MAX    UINT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX

#endif
