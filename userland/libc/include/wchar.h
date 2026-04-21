#ifndef _MAKAOS_WCHAR_H
#define _MAKAOS_WCHAR_H 1

#include <stddef.h>    // compiler-provided wchar_t
#include <stdarg.h>

typedef unsigned int wint_t;
typedef int          mbstate_t;  // single-locale, state-less

#define WEOF ((wint_t)-1)
#define WCHAR_MIN 0
#define WCHAR_MAX 0x7FFFFFFF

// Multibyte ↔ wide-char (MakaOS is UTF-8-only; these are thin passthroughs
// that handle the ASCII-subset fast path).
int    mbtowc(wchar_t* pwc, const char* s, size_t n);
int    wctomb(char* s, wchar_t wc);
size_t mbstowcs(wchar_t* dst, const char* src, size_t n);
size_t wcstombs(char* dst, const wchar_t* src, size_t n);
int    mblen(const char* s, size_t n);

size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps);
size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps);
size_t mbsrtowcs(wchar_t* dst, const char** src, size_t n, mbstate_t* ps);
size_t wcsrtombs(char* dst, const wchar_t** src, size_t n, mbstate_t* ps);
size_t mbrlen(const char* s, size_t n, mbstate_t* ps);
int    mbsinit(const mbstate_t* ps);

// Wide string primitives
size_t  wcslen(const wchar_t* s);
wchar_t* wcschr(const wchar_t* s, wchar_t c);
int      wcscmp(const wchar_t* a, const wchar_t* b);
int      wcsncmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* wcscpy(wchar_t* dst, const wchar_t* src);
wchar_t* wcsncpy(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wcsdup(const wchar_t* s);
wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n);
int      wcscoll(const wchar_t* a, const wchar_t* b);

// Classification (operate on wint_t)
wint_t btowc(int c);
int    wctob(wint_t c);
int    wcwidth(wchar_t c);
int    wcswidth(const wchar_t* s, size_t n);

#endif
