#pragma once
#include "common.h"

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
#define SIG_QUEUE_SIZE 32   // power of 2; max pending signals queued at once

typedef struct {
    uint8_t       queue[SIG_QUEUE_SIZE]; // ring buffer of pending signal numbers
    uint8_t       head;
    uint8_t       tail;
    uint32_t      blocked;               // bitmask: bit (sig-1) set → blocked
    k_sigaction_t handlers[NSIG];        // per-signal user action (0 = SIG_DFL)
    uint64_t      sigframe_rsp;          // address of sigframe_t on user stack
} sigstate_t;

// ── API ───────────────────────────────────────────────────────────────────

struct task_t;

void signal_send(struct task_t* t, int sig);
void signal_send_group(uint32_t tgid, int sig);
void signal_deliver_pending(void);
