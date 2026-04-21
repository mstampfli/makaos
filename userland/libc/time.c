// ── time.c — <time.h> clock APIs ────────────────────────────────────
//
// MakaOS exposes a single monotonic nanosecond clock via SYS_CLOCK_NS.
// Every clock id is routed to it — we don't yet separate wall vs.
// monotonic.  Resolution is 1 ns.

#include <makaos/syscall.h>
#include <time.h>
#include <errno.h>

int clock_gettime(clockid_t id, struct timespec* ts) {
    (void)id;
    if (!ts) { errno = EINVAL; return -1; }
    uint64_t ns = syscall0(SYS_CLOCK_NS);
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)  (ns % 1000000000ull);
    return 0;
}

int clock_getres(clockid_t id, struct timespec* res) {
    (void)id;
    if (!res) { errno = EINVAL; return -1; }
    res->tv_sec  = 0;
    res->tv_nsec = 1;
    return 0;
}
