#pragma once
// stdint.h — provide standard integer types
#include "../../../libc/libc.h"
// All basic types already defined in libc.h; just add the extras below.

typedef int64_t  intptr_t;
typedef uint64_t uintptr_t;
typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;
typedef int64_t  ptrdiff_t;

#define INT8_MIN    (-128)
#define INT8_MAX    127
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define INT32_MIN   (-2147483648)
#define INT32_MAX   2147483647
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL
#define UINT8_MAX   255U
#define UINT16_MAX  65535U
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615ULL

#define SIZE_MAX    UINT64_MAX
#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX

#define INT8_C(c)   c
#define INT16_C(c)  c
#define INT32_C(c)  c
#define INT64_C(c)  c ## LL
#define UINT8_C(c)  c ## U
#define UINT16_C(c) c ## U
#define UINT32_C(c) c ## U
#define UINT64_C(c) c ## ULL
