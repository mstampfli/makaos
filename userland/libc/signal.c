// ── signal.c — <signal.h> externs ───────────────────────────────────
//
// libc.h has these as static inlines for in-tree apps.  Sysroot
// consumers (wayland's event-loop signalfd integration, harfbuzz
// crash handlers, …) need link-resolvable symbols.

#include <makaos/syscall.h>
#include <signal.h>
#include <errno.h>

extern void __sigreturn_trampoline(void);

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    // Install the restorer trampoline for userspace→kernel signal
    // return.  The in-tree inline does the same rewrite; we mirror it
    // here so external callers don't need to know about the contract.
    if (act) {
        struct sigaction fixed = *act;
        fixed.sa_restorer = __sigreturn_trampoline;
        act = &fixed;
    }
    return (int)__syscall_ret(
        syscall3(SYS_SIGACTION, (uint64_t)(uint32_t)sig,
                 (uint64_t)act, (uint64_t)oldact));
}

int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    // Kernel expects (how, set*, oldset*) — sigset_t is uint32_t
    // in our ABI but the public signature uses unsigned long; route
    // through syscall3 unchanged.
    return (int)__syscall_ret(
        syscall3(SYS_SIGPROCMASK,
                 (uint64_t)(uint32_t)how,
                 (uint64_t)set, (uint64_t)oldset));
}

int sigemptyset(sigset_t* s) { if (!s) { errno = EINVAL; return -1; } *s = 0;   return 0; }
int sigfillset(sigset_t* s)  { if (!s) { errno = EINVAL; return -1; } *s = ~0ul; return 0; }

int sigaddset(sigset_t* s, int sig) {
    if (!s || sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    *s |= (sigset_t)(1ul << (sig - 1));
    return 0;
}

int sigdelset(sigset_t* s, int sig) {
    if (!s || sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    *s &= ~(sigset_t)(1ul << (sig - 1));
    return 0;
}

int sigismember(const sigset_t* s, int sig) {
    if (!s || sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    return (*s & (sigset_t)(1ul << (sig - 1))) ? 1 : 0;
}
