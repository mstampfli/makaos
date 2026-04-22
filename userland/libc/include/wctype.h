#ifndef _MAKAOS_WCTYPE_H
#define _MAKAOS_WCTYPE_H 1
// POSIX <wctype.h> — wide-character classification + case mapping.
// Our implementations only cover ASCII; ports that need full Unicode
// classification should use utf8proc (which foot already links
// through fcft).

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// wint_t + WEOF are provided by <wchar.h> (included above).  Just
// the wctype / wctrans identifiers are new here.
typedef int wctype_t;
typedef int wctrans_t;

int iswalpha(wint_t c);
int iswalnum(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);
int iswctype(wint_t c, wctype_t type);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);
wint_t towctrans(wint_t c, wctrans_t trans);
wctype_t  wctype(const char* name);
wctrans_t wctrans(const char* name);

#ifdef __cplusplus
}
#endif

#endif
