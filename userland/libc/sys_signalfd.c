// ── sys_signalfd.c — <sys/signalfd.h> (POSIX/Linux) ─────────────────

#include <makaos/syscall.h>
#include <sys/signalfd.h>
#include <signal.h>

int signalfd(int fd, const sigset_t* mask, int flags) {
    return (int)__syscall_ret(
        syscall3(SYS_SIGNALFD,
                 (uint64_t)(int64_t)fd,
                 (uint64_t)mask,
                 (uint64_t)(unsigned int)flags));
}
