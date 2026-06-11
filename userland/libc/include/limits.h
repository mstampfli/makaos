#ifndef _MAKAOS_LIMITS_H
#define _MAKAOS_LIMITS_H 1

#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX
#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535
#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295u
#define LONG_MIN    (-9223372036854775807L - 1)
#define LONG_MAX    9223372036854775807L
#define ULONG_MAX   18446744073709551615UL
#define LLONG_MIN   (-9223372036854775807LL - 1)
#define LLONG_MAX   9223372036854775807LL
#define ULLONG_MAX  18446744073709551615ULL

// POSIX / MakaOS-specific limits
#define PATH_MAX    4096
#define NAME_MAX    255
#define OPEN_MAX    1024
#define PIPE_BUF    4096
#define HOST_NAME_MAX        64
#define _POSIX_HOST_NAME_MAX 255   // POSIX floor; foot uri.c wants it
#define SSIZE_MAX   LONG_MAX       // ssize_t is 64-bit; glib giounix.c
#define MB_LEN_MAX  4              // UTF-8 worst case

#endif
