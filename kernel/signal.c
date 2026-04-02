#include "signal.h"
#include "process.h"
#include "sched.h"
#include "common.h"

// ── signal_send ───────────────────────────────────────────────────────────
// Sets the pending bit for `sig` on task `t`.
// SIGKILL cannot be blocked — we clear its blocked bit defensively.
// Safe from IRQ context: only writes one bit atomically (single CPU).
void signal_send(task_t* t, int sig) {
    if (!t) return;
    if (sig < 1 || sig >= NSIG) return;

    uint32_t bit = 1u << (uint32_t)(sig - 1);

    // SIGKILL is unblockable — forcibly clear the blocked bit.
    if (sig == SIGKILL)
        t->sigstate.blocked &= ~bit;

    t->sigstate.pending |= bit;

    // Wake the task if it's sleeping so it processes the signal promptly.
    if (t->state == TASK_SLEEPING)
        sched_wake(t);
}

// ── signal_send_group ─────────────────────────────────────────────────────
// Sends `sig` to every task whose tgid matches.
// Walks the run queue and also checks g_current.
// NOTE: this only reaches tasks in the scheduler's queue + current task.
// A task that is TASK_DEAD is skipped.
void signal_send_group(uint32_t tgid, int sig) {
    // Check current task.
    if (g_current && g_current->tgid == tgid)
        signal_send(g_current, sig);

    // Walk run queue.  The queue is a singly-linked list via ->next.
    // We access it directly — single CPU, no races.
    extern task_t* sched_queue_head(void);
    task_t* t = sched_queue_head();
    while (t) {
        if (t->tgid == tgid && t != g_current)
            signal_send(t, sig);
        t = t->next;
    }
}

// ── signal_deliver_pending ────────────────────────────────────────────────
// Processes all pending, unblocked signals for g_current.
// Called on scheduler entry (before a task runs) and on syscall return.
//
// All signals currently terminate the task (no user-space handlers yet).
// SIGTERM prints nothing (graceful). SIGSEGV/SIGFPE/SIGILL print a
// one-line diagnostic to VGA row 0 before terminating.
void signal_deliver_pending(void) {
    if (!g_current || g_current->state == TASK_DEAD) return;

    sigstate_t* ss = &g_current->sigstate;
    uint32_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) return;

    // Find the lowest-numbered pending signal.
    int sig = 0;
    for (int i = 0; i < NSIG - 1; i++) {
        if (deliverable & (1u << i)) { sig = i + 1; break; }
    }
    if (!sig) return;

    // Clear the pending bit.
    ss->pending &= ~(1u << (uint32_t)(sig - 1));

    // Print diagnostic for fatal hardware signals.
    extern volatile uint16_t* g_vga;
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL) {
        static const char* names[] = {
            [SIGSEGV] = "SIGSEGV",
            [SIGBUS]  = "SIGBUS ",
            [SIGFPE]  = "SIGFPE ",
            [SIGILL]  = "SIGILL ",
        };
        const char* name = (sig < NSIG && names[sig]) ? names[sig] : "SIG???";
        // Print "pid NNN: <SIGNAME> - killed" on VGA row 0, red on black.
        volatile uint16_t* v = g_vga;
        const uint16_t attr = (uint16_t)(0x04 << 8); // red on black
        uint32_t col = 0;

        // "pid "
        const char* prefix = "pid ";
        for (int i = 0; prefix[i]; i++) v[col++] = (uint16_t)(uint8_t)prefix[i] | attr;

        // pid number
        uint32_t pid = g_current->pid;
        char pidbuf[12]; int pi = 11; pidbuf[pi] = 0;
        do { pidbuf[--pi] = '0' + (char)(pid % 10); pid /= 10; } while (pid);
        for (int i = pi; pidbuf[i]; i++) v[col++] = (uint16_t)(uint8_t)pidbuf[i] | attr;

        const char* mid = ": ";
        for (int i = 0; mid[i]; i++) v[col++] = (uint16_t)(uint8_t)mid[i] | attr;
        for (int i = 0; name[i]; i++) v[col++] = (uint16_t)(uint8_t)name[i] | attr;
        const char* suffix = " - killed";
        for (int i = 0; suffix[i]; i++) v[col++] = (uint16_t)(uint8_t)suffix[i] | attr;
    }

    // All signals: terminate the task.
    g_current->state = TASK_DEAD;
    sched_yield();
    // Never reached.
    for (;;) __asm__ volatile("hlt");
}
