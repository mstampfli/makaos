// ── sys_time.c — <sys/time.h> ───────────────────────────────────────
//
// Backed by SYS_CLOCK_NS — MakaOS has one monotonic nanosecond clock.
// `tz` is accepted for compatibility and ignored (POSIX deprecated it).

#include <makaos/syscall.h>
#include <sys/time.h>
#include <errno.h>

int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    if (!tv) { errno = EINVAL; return -1; }
    uint64_t ns = syscall0(SYS_CLOCK_NS);
    tv->tv_sec  = (time_t)     (ns / 1000000000ull);
    tv->tv_usec = (suseconds_t)((ns % 1000000000ull) / 1000ull);
    return 0;
}

// utimes / lutimes — POSIX timestamp update.  MakaOS virtfs has no
// per-inode timestamp write path yet, so both are success-stubs.
// Callers (fontconfig cache, build-system touch-like logic) treat
// these as hints, never as correctness-critical.
// TODO(scalability-debt-ledger-#5): SYS_UTIMES → virtfs inode atime/
// mtime write path.
int utimes(const char* path, const struct timeval times[2]) {
    (void)path; (void)times;
    return 0;
}

int lutimes(const char* path, const struct timeval times[2]) {
    (void)path; (void)times;
    return 0;
}
