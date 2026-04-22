// ── wctype.c — ASCII-only <wctype.h> ────────────────────────────────
//
// Minimal impl: anything outside ASCII (< 0x80) falls through to
// "not a letter, not a digit, no case change".  Foot's use of these
// is confined to handling control sequences + printable classification
// within the 7-bit range, so ASCII covers the hot path.  Full Unicode
// classification routes through utf8proc (fcft already links it).

#include <wctype.h>
#include <ctype.h>

int iswalpha(wint_t c)  { return c < 0x80 && isalpha((int)c);  }
int iswalnum(wint_t c)  { return c < 0x80 && isalnum((int)c);  }
int iswblank(wint_t c)  { return c == ' ' || c == '\t';        }
int iswcntrl(wint_t c)  { return c < 0x80 && iscntrl((int)c);  }
int iswdigit(wint_t c)  { return c < 0x80 && isdigit((int)c);  }
int iswgraph(wint_t c)  { return c < 0x80 && isgraph((int)c);  }
int iswlower(wint_t c)  { return c < 0x80 && islower((int)c);  }
int iswprint(wint_t c)  { return (c >= 0x20 && c < 0x7F) || c >= 0x80; }
int iswpunct(wint_t c)  { return c < 0x80 && ispunct((int)c);  }
int iswspace(wint_t c)  { return c < 0x80 && isspace((int)c);  }
int iswupper(wint_t c)  { return c < 0x80 && isupper((int)c);  }
int iswxdigit(wint_t c) { return c < 0x80 && isxdigit((int)c); }

int iswctype(wint_t c, wctype_t type) { (void)type; return iswalnum(c); }

wint_t towlower(wint_t c) { return c < 0x80 ? (wint_t)tolower((int)c) : c; }
wint_t towupper(wint_t c) { return c < 0x80 ? (wint_t)toupper((int)c) : c; }
wint_t towctrans(wint_t c, wctrans_t trans) { (void)trans; return c; }

wctype_t  wctype(const char* name)  { (void)name; return 0; }
wctrans_t wctrans(const char* name) { (void)name; return 0; }
