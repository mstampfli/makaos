// ── select.c — <sys/select.h> wrapper (SYS_SELECT) ──────────────────
#include <makaos/syscall.h>
#include <sys/select.h>

int select(int nfds, fd_set* rd, fd_set* wr, fd_set* er, struct timeval* tv) {
    return (int)__syscall_ret(
        syscall5(SYS_SELECT, (uint64_t)(int64_t)nfds, (uint64_t)rd,
                 (uint64_t)wr, (uint64_t)er, (uint64_t)tv));
}
