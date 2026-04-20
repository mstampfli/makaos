#ifndef _MAKAOS_LANGINFO_H
#define _MAKAOS_LANGINFO_H 1

#include <locale.h>

// nl_langinfo items.  Only a subset is implemented — nl_langinfo
// always returns "UTF-8" for CODESET and the C-locale strings for
// the rest.  Enough for fontconfig, ICU probing, curl.

typedef int nl_item;

#define CODESET       14
#define D_T_FMT       131
#define D_FMT         132
#define T_FMT         133
#define T_FMT_AMPM    134
#define AM_STR        135
#define PM_STR        136
#define DAY_1         137
#define DAY_2         138
#define DAY_3         139
#define DAY_4         140
#define DAY_5         141
#define DAY_6         142
#define DAY_7         143
#define ABDAY_1       144
#define ABDAY_2       145
#define ABDAY_3       146
#define ABDAY_4       147
#define ABDAY_5       148
#define ABDAY_6       149
#define ABDAY_7       150
#define MON_1         157
#define MON_2         158
#define MON_3         159
#define MON_4         160
#define MON_5         161
#define MON_6         162
#define MON_7         163
#define MON_8         164
#define MON_9         165
#define MON_10        166
#define MON_11        167
#define MON_12        168
#define ABMON_1       169
#define ABMON_2       170
#define ABMON_3       171
#define ABMON_4       172
#define ABMON_5       173
#define ABMON_6       174
#define ABMON_7       175
#define ABMON_8       176
#define ABMON_9       177
#define ABMON_10      178
#define ABMON_11      179
#define ABMON_12      180
#define RADIXCHAR     0x10000
#define THOUSEP       0x10001
#define YESEXPR       0x50000
#define NOEXPR        0x50001

char* nl_langinfo(nl_item item);

#endif
