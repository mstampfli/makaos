// ── dl_locale.c — external-symbol stubs for <dlfcn.h>, <locale.h>, <langinfo.h>
//
// MakaOS statically links every binary.  The dlopen family and locale
// APIs exist only so ports (mbedTLS, fontconfig, harfbuzz, curl, ICU,
// eventually libwayland) compiled against the Linux-style split
// headers can resolve their imports at link time.  All of these
// return a safe, canonical "C / UTF-8" answer.
//
// libc.h also defines these as `static inline` for legacy in-tree
// apps that include libc.h directly.  Static-inline does not emit an
// external symbol, so these .c definitions win for sysroot consumers
// and both paths coexist without conflict.

#include <stddef.h>
#include <errno.h>
#include <dlfcn.h>
#include <locale.h>
#include <langinfo.h>

// ── dlfcn ────────────────────────────────────────────────────────────
// All operations fail predictably: dlopen returns NULL and dlerror
// returns a fixed diagnostic.  Ports that probe via dlopen degrade
// gracefully (e.g. mbedTLS's optional PKCS11, fontconfig's optional
// language probes) because NULL means "not available".

static const char s_dl_msg[] = "dlopen: dynamic loading is not supported on MakaOS";

void* dlopen(const char* file, int flags) {
    (void)file; (void)flags;
    return NULL;
}

void* dlsym(void* handle, const char* name) {
    (void)handle; (void)name;
    return NULL;
}

int dlclose(void* handle) { (void)handle; return 0; }

char* dlerror(void) { return (char*)s_dl_msg; }

int dladdr(const void* addr, Dl_info* info) {
    (void)addr;
    if (info) {
        info->dli_fname = NULL;
        info->dli_fbase = NULL;
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
    }
    return 0;
}

// ── locale ───────────────────────────────────────────────────────────
// Canonical name returned by setlocale — matches what glibc returns
// for the plain C locale with UTF-8 ctype.

static const char s_locale_name[] = "C.UTF-8";

// Conservative struct lconv — numeric fields point to empty strings
// except decimal_point / thousands_sep which are set per POSIX C locale.
static char s_dp[]  = ".";
static char s_ts[]  = "";
static char s_gg[]  = "";
static char s_sgn[] = "";

static struct lconv s_lconv = {
    .decimal_point       = s_dp,
    .thousands_sep       = s_ts,
    .grouping            = s_gg,
    .int_curr_symbol     = s_ts,
    .currency_symbol     = s_ts,
    .mon_decimal_point   = s_ts,
    .mon_thousands_sep   = s_ts,
    .mon_grouping        = s_gg,
    .positive_sign       = s_sgn,
    .negative_sign       = s_sgn,
    .int_frac_digits     = (char)127,
    .frac_digits         = (char)127,
    .p_cs_precedes       = (char)127,
    .p_sep_by_space      = (char)127,
    .n_cs_precedes       = (char)127,
    .n_sep_by_space      = (char)127,
    .p_sign_posn         = (char)127,
    .n_sign_posn         = (char)127,
    .int_p_cs_precedes   = (char)127,
    .int_p_sep_by_space  = (char)127,
    .int_n_cs_precedes   = (char)127,
    .int_n_sep_by_space  = (char)127,
    .int_p_sign_posn     = (char)127,
    .int_n_sign_posn     = (char)127,
};

char* setlocale(int category, const char* locale) {
    (void)category; (void)locale;
    return (char*)s_locale_name;
}

struct lconv* localeconv(void) { return &s_lconv; }

locale_t newlocale(int mask, const char* locale, locale_t base) {
    (void)mask; (void)locale; (void)base;
    return (locale_t)1;   // opaque, non-NULL handle
}

locale_t duplocale(locale_t loc) { return loc ? loc : (locale_t)1; }

void freelocale(locale_t loc) { (void)loc; }

locale_t uselocale(locale_t loc) { (void)loc; return LC_GLOBAL_LOCALE; }

// ── langinfo ─────────────────────────────────────────────────────────
// Minimal but honest: CODESET = UTF-8, everything else = C locale.
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
