#ifndef _MAKAOS_UCHAR_H
#define _MAKAOS_UCHAR_H 1
// C11 <uchar.h> — char16_t / char32_t types + UTF conversion helpers.
// foot uses char32_t for its glyph cache.  We don't ship full multi-
// byte conversion (c32rtomb / mbrtoc32) — foot doesn't call them on
// the rendering hot path; when a port does need them we'll fill in.

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __CHAR16_TYPE__
typedef unsigned short  char16_t;
#else
typedef __CHAR16_TYPE__ char16_t;
#endif
#ifndef __CHAR32_TYPE__
typedef unsigned int    char32_t;
#else
typedef __CHAR32_TYPE__ char32_t;
#endif

size_t mbrtoc16(char16_t* pc16, const char* s, size_t n, mbstate_t* ps);
size_t c16rtomb(char* s, char16_t c16, mbstate_t* ps);
size_t mbrtoc32(char32_t* pc32, const char* s, size_t n, mbstate_t* ps);
size_t c32rtomb(char* s, char32_t c32, mbstate_t* ps);

#ifdef __cplusplus
}
#endif

#endif
