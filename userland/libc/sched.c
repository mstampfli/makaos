// sched.c - CPU affinity (sched_getaffinity family).
//
// MakaOS has no affinity control; a thread may run on any online CPU. So
// sched_getaffinity reports every online CPU as available (which is what the
// affinity API is really used for: counting/inspecting the usable CPUs) and
// sched_setaffinity accepts requests silently. TODO: honour affinity if the
// scheduler ever gains per-CPU pinning.

#include <sched.h>
#include <unistd.h>
#include <errno.h>

int CPU_COUNT(const cpu_set_t* s) {
    int n = 0;
    // Count set bits without __builtin_popcountl (which would pull libgcc's
    // __popcountdi2, not present in libc.a). Kernighan's bit-clearing loop.
    for (unsigned i = 0; i < sizeof(s->__bits) / sizeof(s->__bits[0]); i++) {
        unsigned long w = s->__bits[i];
        while (w) { w &= (w - 1); n++; }
    }
    return n;
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask) {
    (void)pid;
    if (!mask || cpusetsize < sizeof(cpu_set_t)) { errno = EINVAL; return -1; }
    CPU_ZERO(mask);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    for (long i = 0; i < n && i < CPU_SETSIZE; i++) CPU_SET(i, mask);
    return 0;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t* mask) {
    (void)pid; (void)cpusetsize; (void)mask;
    return 0;
}
