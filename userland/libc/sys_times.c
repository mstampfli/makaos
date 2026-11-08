// sys_times.c - <sys/times.h>.
//
// MakaOS has one monotonic clock and no per-process CPU accounting yet, so we
// report elapsed monotonic time (in CLK_TCK ticks) as the return value and zero
// the user/system breakdown. POSIX only guarantees the return value is a tick
// count from an arbitrary epoch usable for measuring elapsed intervals, which
// is exactly what callers like OpenJDK use it for. TODO: fill tms_[u|s]time
// from a real per-process CPU clock when the scheduler exposes one.

#include <sys/times.h>
#include <time.h>
#include <unistd.h>

clock_t times(struct tms* buf) {
    if (buf) {
        buf->tms_utime = 0;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return (clock_t)-1;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    return (clock_t)((long long)ts.tv_sec * hz + ts.tv_nsec / (1000000000L / hz));
}
