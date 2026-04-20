#pragma once
#include "libc.h"

// Ensure every CHAR/SHORT/INT/LONG *_MAX mbedTLS pokes at is defined.
#ifndef CHAR_BIT
  #define CHAR_BIT 8
#endif
#ifndef SCHAR_MIN
  #define SCHAR_MIN (-128)
#endif
#ifndef SCHAR_MAX
  #define SCHAR_MAX 127
#endif
#ifndef UCHAR_MAX
  #define UCHAR_MAX 255
#endif
#ifndef CHAR_MIN
  #define CHAR_MIN SCHAR_MIN
#endif
#ifndef CHAR_MAX
  #define CHAR_MAX SCHAR_MAX
#endif
#ifndef SHRT_MIN
  #define SHRT_MIN (-32768)
#endif
#ifndef SHRT_MAX
  #define SHRT_MAX 32767
#endif
#ifndef USHRT_MAX
  #define USHRT_MAX 65535
#endif
#ifndef INT_MIN
  #define INT_MIN (-2147483647 - 1)
#endif
#ifndef INT_MAX
  #define INT_MAX 2147483647
#endif
#ifndef UINT_MAX
  #define UINT_MAX 4294967295u
#endif
#ifndef LONG_MAX
  #define LONG_MAX 9223372036854775807L
#endif
#ifndef LONG_MIN
  #define LONG_MIN (-LONG_MAX - 1L)
#endif
#ifndef ULONG_MAX
  #define ULONG_MAX 18446744073709551615UL
#endif
#ifndef LLONG_MAX
  #define LLONG_MAX 9223372036854775807LL
#endif
#ifndef LLONG_MIN
  #define LLONG_MIN (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
  #define ULLONG_MAX 18446744073709551615ULL
#endif
