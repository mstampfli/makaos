#ifndef _MAKAOS_STDDEF_H
#define _MAKAOS_STDDEF_H 1
// ISO C stddef.h — size_t, ptrdiff_t, NULL, offsetof, max_align_t, wchar_t.
//
// These types are ABI-fixed on x86_64 System V and must agree with the
// compiler's __SIZE_TYPE__ / __PTRDIFF_TYPE__.  They do.

typedef unsigned long       size_t;
typedef   signed long       ptrdiff_t;
// wchar_t is a built-in type in C++, not typedefable; only declare
// in C.  (Cross-language headers must wrap this.)
#ifndef __cplusplus
typedef int                 wchar_t;
#endif

// ssize_t is technically a POSIX type but libgcc pulls it via stdio.h
// in freestanding compiles, so expose it here too with the shared
// guard to keep a single source of truth.
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED 1
typedef long ssize_t;
#endif

typedef struct { long long __ll; long double __ld; } max_align_t;

#ifndef NULL
  #define NULL ((void*)0)
#endif

#define offsetof(t, m) __builtin_offsetof(t, m)

#endif
