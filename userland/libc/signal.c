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

// sigtimedwait / sigwaitinfo / sigwait — synchronous signal reception.
//
// The MakaOS kernel doesn't yet expose SYS_RT_SIGTIMEDWAIT.  Provide
// stubs that fail with EAGAIN / EINVAL so upstream callers (SDL3's
// Wayland clipboard pump, a few glib paths) take their error-path
// fallback instead of blocking forever.  When the kernel grows a
// rt_sigtimedwait syscall these become thin wrappers.
struct timespec;
int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout) {
    (void)set; (void)info; (void)timeout;
    errno = EAGAIN; return -1;
}
int sigwaitinfo(const sigset_t* set, siginfo_t* info) {
    (void)set; (void)info;
    errno = EINVAL; return -1;
}
int sigwait(const sigset_t* set, int* sig) {
    (void)set; (void)sig;
    return EINVAL;   // POSIX sigwait returns errno directly, not -1
}

// signal() — legacy API, implement as sigaction wrapper.  MakaOS's
// sigaction already installs the restorer trampoline, so this just
// fills in a struct and forwards.
void (*signal(int sig, void (*handler)(int)))(int) {
    struct sigaction act = {0}, old = {0};
    act.sa_handler = handler;
    act.sa_flags   = SA_RESTART;
    sigemptyset(&act.sa_mask);
    if (sigaction(sig, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}
