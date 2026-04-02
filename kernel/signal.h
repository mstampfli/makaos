#pragma once
#include "common.h"

// ── Signal numbers ────────────────────────────────────────────────────────
// Mirrors POSIX signal numbers where it matters for future compatibility.
#define SIGTERM   15   // graceful shutdown request (can be caught/ignored)
#define SIGKILL    9   // immediate kill (unblockable, uncatchable)
#define SIGSEGV   11   // invalid memory access
#define SIGBUS     7   // bus error (alignment fault, etc.)
#define SIGFPE     8   // arithmetic error (divide-by-zero, etc.)
#define SIGILL     4   // illegal instruction

#define NSIG       32  // total signal slots (bits in a uint32_t mask)

// ── Signal actions ────────────────────────────────────────────────────────
// What to do when a signal is delivered and no user handler is installed.
// We only support default actions for now (no user-space signal handlers yet —
// that requires saving/restoring a full user register frame, which comes with
// the ELF/ring-3 work).
#define SIG_DFL    0   // default action
#define SIG_IGN    1   // ignore

// Default action for each signal:
//   SIGTERM → TERM (mark dead, yield — graceful)
//   SIGKILL → TERM (same outcome but unblockable)
//   SIGSEGV → TERM + print diagnostic
//   SIGFPE  → TERM + print diagnostic
//   SIGILL  → TERM + print diagnostic

// ── Per-task signal state (embedded in task_t) ────────────────────────────
typedef struct {
    uint32_t pending;   // bitmask of pending signals (bit N = signal N+1)
    uint32_t blocked;   // bitmask of blocked signals (SIGKILL/SIGSTOP always 0 here)
} sigstate_t;

// ── API ───────────────────────────────────────────────────────────────────

struct task_t;

// Send signal `sig` to task `t`.
// SIGKILL cannot be blocked.  Safe to call from IRQ context (just sets a bit).
void signal_send(struct task_t* t, int sig);

// Send signal `sig` to all tasks in thread group `tgid`.
void signal_send_group(uint32_t tgid, int sig);

// Check and deliver any pending signals for the current task.
// Called on every scheduler entry and on syscall return.
// May not return if the signal kills the task (calls sched_yield instead).
void signal_deliver_pending(void);
