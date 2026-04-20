#pragma once
#include "libc.h"

// mbedTLS checks these MAX macros; libc.h may not define every one.
#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t)-1)
#endif
#ifndef UINT32_MAX
  #define UINT32_MAX 0xFFFFFFFFu
#endif
#ifndef UINT64_MAX
  #define UINT64_MAX 0xFFFFFFFFFFFFFFFFull
#endif
#ifndef INT32_MAX
  #define INT32_MAX 0x7FFFFFFF
#endif
#ifndef INT64_MAX
  #define INT64_MAX 0x7FFFFFFFFFFFFFFFll
#endif
#ifndef INT32_MIN
  #define INT32_MIN (-INT32_MAX - 1)
#endif
#ifndef INT64_MIN
  #define INT64_MIN (-INT64_MAX - 1ll)
#endif
#ifndef UINT8_MAX
  #define UINT8_MAX  0xFFu
#endif
#ifndef UINT16_MAX
  #define UINT16_MAX 0xFFFFu
#endif
