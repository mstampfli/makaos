// ── poll.c — <poll.h> wrapper ───────────────────────────────────────
#include <makaos/syscall.h>
#include <poll.h>

int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    return (int)__syscall_ret(
        syscall3(SYS_POLL, (uint64_t)fds, (uint64_t)nfds, (uint64_t)(int64_t)timeout_ms));
}
