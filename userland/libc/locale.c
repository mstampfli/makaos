// ── locale.c — POSIX locale stubs ────────────────────────────────────
//
// MakaOS supports only the C / C.UTF-8 locale.  setlocale accepts any
// name and returns the canonical string; newlocale/etc. return an
// opaque non-NULL handle that's safe to ignore.

#include <locale.h>

static const char s_locale_name[] = "C.UTF-8";

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

char*         setlocale(int cat, const char* loc) { (void)cat; (void)loc; return (char*)s_locale_name; }
struct lconv* localeconv(void)                    { return &s_lconv; }
locale_t      newlocale(int m, const char* l, locale_t b) { (void)m; (void)l; (void)b; return (locale_t)1; }
locale_t      duplocale(locale_t l)               { return l ? l : (locale_t)1; }
void          freelocale(locale_t l)              { (void)l; }
locale_t      uselocale(locale_t l)               { (void)l; return LC_GLOBAL_LOCALE; }
