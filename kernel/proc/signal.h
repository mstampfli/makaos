#pragma once
#include "common.h"
#include "smp.h"

// ── Signal numbers ────────────────────────────────────────────────────────
// Mirrors POSIX signal numbers where it matters for future compatibility.
#define SIGHUP     1   // hangup
#define SIGINT     2   // terminal interrupt (Ctrl-C)
#define SIGQUIT    3   // terminal quit (Ctrl-\)
#define SIGILL     4   // illegal instruction
#define SIGTRAP    5   // trace/breakpoint trap
#define SIGABRT    6   // abort
#define SIGBUS     7   // bus error (alignment fault, etc.)
#define SIGFPE     8   // arithmetic error (divide-by-zero, etc.)
#define SIGKILL    9   // immediate kill (unblockable, uncatchable)
#define SIGUSR1   10   // user-defined signal 1
#define SIGSEGV   11   // invalid memory access
#define SIGUSR2   12   // user-defined signal 2
#define SIGPIPE   13   // broken pipe (write to closed reader)
#define SIGALRM   14   // alarm clock
#define SIGTERM   15   // graceful shutdown request (can be caught/ignored)
#define SIGCHLD   17   // child stopped or terminated
#define SIGCONT   18   // continue if stopped
#define SIGSTOP   19   // stop process (unblockable, uncatchable)
#define SIGTSTP   20   // terminal stop (Ctrl-Z, catchable)
#define SIGTTIN   21   // background read from terminal
#define SIGTTOU   22   // background write to terminal
#define SIGWINCH  28   // terminal window size changed

#define NSIG      32   // total signal slots (bits in a uint32_t mask)

// ── Signal actions ────────────────────────────────────────────────────────
#define SIG_DFL    0   // default action
#define SIG_IGN    1   // ignore

// ── sigprocmask how values ────────────────────────────────────────────────
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

// ── sigaction flags ───────────────────────────────────────────────────────
#define SA_RESTORER  0x04000000  // sa_restorer field is valid

// ── Per-signal kernel action ──────────────────────────────────────────────
// sa_handler: 0 = SIG_DFL, 1 = SIG_IGN, else user function pointer.
// sa_restorer: user-space trampoline that calls sigreturn (required).
// sa_mask:    additional signals to block while handler runs.
typedef struct {
    uint64_t sa_handler;
    uint64_t sa_restorer;
    uint32_t sa_mask;
    uint32_t sa_flags;
} k_sigaction_t;

// ── Signal frame saved on the user stack during handler delivery ──────────
// Laid out at (user_rsp - sizeof(sigframe_t)) & ~0xF before calling handler.
// sys_sigreturn reads this to restore the interrupted context.
typedef struct {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint32_t blocked;   // signal mask at time of delivery
    uint32_t _pad;
} sigframe_t;

// ── Per-task signal state (embedded in task_t) ────────────────────────────
//
// Pending signals are represented as a bitmap: bit (sig-1) in `pending` is
// set iff signal `sig` is queued for delivery.  This is the classic POSIX
// semantics for non-RT signals: sending the same signal twice while it's
// blocked coalesces into a single delivery (SIGCHLD, SIGINT, etc.).
//
// With NSIG=32 the bitmap fits in a single uint32_t, so set/clear/scan are
// O(1).  atomic_set_bit/atomic_clear_bit are used so senders on other CPUs
// (future SMP) don't race with the receiver's dequeue path.
//
// Real-time signals (SIGRTMIN..SIGRTMAX) require a *queue* with payloads;
// MakaOS does not support them and is unlikely to.  If added later, hang a
// separate sigqueue_t list off sigstate_t for sig >= 32 only.
typedef struct {
    volatile uint32_t pending;           // bitmap of pending signals (1<<(sig-1))
    uint32_t          blocked;           // bitmap of blocked signals
    k_sigaction_t     handlers[NSIG];    // per-signal user action (0 = SIG_DFL)
    uint64_t          sigframe_rsp;      // address of sigframe_t on user stack
} sigstate_t;

// ── API ───────────────────────────────────────────────────────────────────

struct task_t;

void signal_send(struct task_t* t, int sig);
void signal_send_group(uint32_t tgid, int sig);
void signal_send_pgrp(uint32_t pgid, int sig);
void signal_deliver_pending(void);

// Mask of signals whose POSIX SIG_DFL action is "ignore".  If one of these
// is pending with SIG_DFL, signal_deliver_pending silently drops it and
// blocking syscalls must NOT return EINTR for it (that was the SIGCHLD
// infinite EINTR loop bug in login — see feedback_sigchld_eintr.md).
#define SIG_DFL_IGNORE_MASK  ((1u << (SIGCHLD - 1)) | (1u << (SIGWINCH - 1)))

// Returns 1 if there is a pending signal that would actually interrupt a
// blocking syscall (has a user handler, or SIG_DFL with non-ignore default).
static inline int signal_has_actionable(const sigstate_t* ss) {
    uint32_t eff = ss->pending & ~ss->blocked;
    if (!eff) return 0;
    // A bit is actionable iff its handler is not SIG_IGN and it's not a
    // SIG_DFL-ignored signal (SIGCHLD/SIGWINCH with SIG_DFL).
    uint32_t mask = eff;
    while (mask) {
        int bit = __builtin_ctz(mask);
        mask &= mask - 1;
        int sig = bit + 1;
        uint64_t h = ss->handlers[sig].sa_handler;
        if (h == (uint64_t)SIG_IGN) continue;
        if (h == (uint64_t)SIG_DFL && (SIG_DFL_IGNORE_MASK & (1u << bit))) continue;
        return 1;
    }
    return 0;
}
