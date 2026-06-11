// ── signal.c — <signal.h> externs ───────────────────────────────────
//
// libc.h has these as static inlines for in-tree apps.  Sysroot
// consumers (wayland's event-loop signalfd integration, harfbuzz
// crash handlers, …) need link-resolvable symbols.

#include <makaos/syscall.h>
#include <signal.h>
#include <errno.h>

extern void __sigreturn_trampoline(void);

// The KERNEL ABI for SYS_SIGACTION (k_sigaction_t in kernel/proc/
// signal.h): handler, restorer, mask32, flags32 — 24 bytes.  The
// public <signal.h> struct sigaction uses the glibc layout (handler,
// mask, flags, restorer).  Passing the public struct straight to the
// kernel made it read sa_restorer from the MASK field — for the usual
// empty mask that registered restorer = 0, and the kernel's
// handler-validation killed the process on the FIRST real delivery
// (dwl died of SIGKILL the moment foot exited and SIGCHLD fired).
// Marshal explicitly; never pass the public struct to the kernel.
typedef struct {
    uint64_t handler;
    uint64_t restorer;
    uint32_t mask;
    uint32_t flags;
} k_sigaction_abi_t;

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    k_sigaction_abi_t kact, kold;
    k_sigaction_abi_t* kap = 0;
    if (act) {
        kact.handler  = (uint64_t)act->sa_handler;
        kact.restorer = (uint64_t)__sigreturn_trampoline;
        kact.mask     = (uint32_t)act->sa_mask;
        kact.flags    = (uint32_t)act->sa_flags;
        kap = &kact;
    }
    long r = (long)__syscall_ret(
        syscall3(SYS_SIGACTION, (uint64_t)(uint32_t)sig,
                 (uint64_t)kap, (uint64_t)(oldact ? &kold : 0)));
    if (r == 0 && oldact) {
        oldact->sa_handler  = (void (*)(int))kold.handler;
        oldact->sa_mask     = (sigset_t)kold.mask;
        oldact->sa_flags    = (int)kold.flags;
        oldact->sa_restorer = (void (*)(void))kold.restorer;
    }
    return (int)r;
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
