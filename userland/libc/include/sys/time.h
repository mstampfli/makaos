#ifndef _MAKAOS_SYS_TIME_H
#define _MAKAOS_SYS_TIME_H 1

#include <sys/select.h>
#include <time.h>

int gettimeofday(struct timeval* tv, void* tz);

// POSIX.1 utimes / lutimes — set access+modification times.  `times`
// is { atime, mtime } or NULL for "now".  MakaOS stubs both as no-ops
// (filesystem has no per-inode timestamp updates yet).  Callers treat
// success/failure as "cache hint", never as data correctness.
int utimes(const char* path, const struct timeval times[2]);
int lutimes(const char* path, const struct timeval times[2]);

// BSD timeval comparison macros — real tools of the trade for
// fontconfig/libdrm/misc. `CMP` is a C relational operator token.
#define timerisset(t)       ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t)       ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, CMP) \
    (((a)->tv_sec == (b)->tv_sec) \
        ? ((a)->tv_usec CMP (b)->tv_usec) \
        : ((a)->tv_sec  CMP (b)->tv_sec))
#define timeradd(a, b, r)                                   \
    do {                                                    \
        (r)->tv_sec  = (a)->tv_sec  + (b)->tv_sec;          \
        (r)->tv_usec = (a)->tv_usec + (b)->tv_usec;         \
        if ((r)->tv_usec >= 1000000) {                      \
            (r)->tv_sec  += 1;                              \
            (r)->tv_usec -= 1000000;                        \
        }                                                   \
    } while (0)
#define timersub(a, b, r)                                   \
    do {                                                    \
        (r)->tv_sec  = (a)->tv_sec  - (b)->tv_sec;          \
        (r)->tv_usec = (a)->tv_usec - (b)->tv_usec;         \
        if ((r)->tv_usec < 0) {                             \
            (r)->tv_sec  -= 1;                              \
            (r)->tv_usec += 1000000;                        \
        }                                                   \
    } while (0)

#endif
