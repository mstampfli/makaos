// ── sys_epoll.c — <sys/epoll.h> externs ─────────────────────────────

#include <makaos/syscall.h>
#include <sys/epoll.h>

int epoll_create(int size) {
    // size is a hint; kernel ignores but POSIX requires >0.
    (void)size;
    return (int)__syscall_ret(syscall1(SYS_EPOLL_CREATE, 0));
}

int epoll_create1(int flags) {
    return (int)__syscall_ret(syscall1(SYS_EPOLL_CREATE, (uint64_t)(unsigned)flags));
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
    return (int)__syscall_ret(
        syscall4(SYS_EPOLL_CTL,
                 (uint64_t)epfd, (uint64_t)op, (uint64_t)fd, (uint64_t)ev));
}

int epoll_wait(int epfd, struct epoll_event* evs, int max, int timeout) {
    return (int)__syscall_ret(
        syscall4(SYS_EPOLL_WAIT,
                 (uint64_t)epfd, (uint64_t)evs,
                 (uint64_t)max, (uint64_t)(int64_t)timeout));
}
