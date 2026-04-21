// ── langinfo.c — nl_langinfo ─────────────────────────────────────────
//
// Minimal but honest: CODESET = UTF-8, everything else = C locale.

#include <langinfo.h>

static const char s_utf8[]        = "UTF-8";
static const char s_empty[]       = "";
static const char s_yesexpr[]     = "^[yY]";
static const char s_noexpr[]      = "^[nN]";
static const char s_radix_point[] = ".";
static const char s_am[]          = "AM";
static const char s_pm[]          = "PM";
static const char s_d_t_fmt[]     = "%a %b %e %H:%M:%S %Y";
static const char s_d_fmt[]       = "%m/%d/%y";
static const char s_t_fmt[]       = "%H:%M:%S";
static const char s_t_fmt_ampm[]  = "%I:%M:%S %p";

char* nl_langinfo(nl_item item) {
    switch (item) {
    case CODESET:    return (char*)s_utf8;
    case RADIXCHAR:  return (char*)s_radix_point;
    case THOUSEP:    return (char*)s_empty;
    case YESEXPR:    return (char*)s_yesexpr;
    case NOEXPR:     return (char*)s_noexpr;
    case AM_STR:     return (char*)s_am;
    case PM_STR:     return (char*)s_pm;
    case D_T_FMT:    return (char*)s_d_t_fmt;
    case D_FMT:      return (char*)s_d_fmt;
    case T_FMT:      return (char*)s_t_fmt;
    case T_FMT_AMPM: return (char*)s_t_fmt_ampm;
    default:         return (char*)s_empty;
    }
}
