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

// clock_nanosleep -- relative (flags==0) or absolute (TIMER_ABSTIME) sleep.
// POSIX: returns 0 on success or a positive errno (it does NOT set errno /
// return -1 the way nanosleep does).  MakaOS has one relative sleep primitive
// (nanosleep -> SYS_NANOSLEEP); for TIMER_ABSTIME we turn the absolute deadline
// into a relative interval off the clock's current value.  A deadline already
// in the past sleeps for zero time.
int clock_nanosleep(clockid_t id, int flags,
                    const struct timespec* req, struct timespec* rem) {
    if (!req) return EINVAL;
    if (flags & TIMER_ABSTIME) {
        struct timespec now, d;
        if (clock_gettime(id, &now) != 0) return EINVAL;
        d.tv_sec  = req->tv_sec  - now.tv_sec;
        d.tv_nsec = req->tv_nsec - now.tv_nsec;
        if (d.tv_nsec < 0) { d.tv_nsec += 1000000000L; d.tv_sec--; }
        if (d.tv_sec < 0 || (d.tv_sec == 0 && d.tv_nsec <= 0)) return 0;
        // Absolute deadline is fixed, so there is no "remaining" to report.
        return nanosleep(&d, 0) == 0 ? 0 : errno;
    }
    return nanosleep(req, rem) == 0 ? 0 : errno;
}

// ── Timezone (UTC) ───────────────────────────────────────────────────
// MakaOS has no timezone database, so it operates in UTC. tzset() is a
// compatibility no-op that (re)asserts the UTC globals; TZ is ignored.
char* tzname[2] = { (char*)"UTC", (char*)"UTC" };
long  timezone  = 0;
int   daylight  = 0;

void tzset(void) {
    tzname[0] = (char*)"UTC";
    tzname[1] = (char*)"UTC";
    timezone  = 0;
    daylight  = 0;
}
