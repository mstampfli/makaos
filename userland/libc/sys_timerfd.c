// ── sys_timerfd.c — <sys/timerfd.h> (Linux-compatible) ──────────────

#include <makaos/syscall.h>
#include <sys/timerfd.h>

int timerfd_create(int clockid, int flags) {
    return (int)__syscall_ret(
        syscall2(SYS_TIMERFD_CREATE, (uint64_t)clockid, (uint64_t)(unsigned int)flags));
}

int timerfd_settime(int fd, int flags,
                    const struct itimerspec* new_value,
                    struct itimerspec* old_value) {
    return (int)__syscall_ret(
        syscall4(SYS_TIMERFD_SETTIME,
                 (uint64_t)fd, (uint64_t)(unsigned int)flags,
                 (uint64_t)new_value, (uint64_t)old_value));
}

int timerfd_gettime(int fd, struct itimerspec* curr_value) {
    return (int)__syscall_ret(
        syscall2(SYS_TIMERFD_GETTIME, (uint64_t)fd, (uint64_t)curr_value));
}
