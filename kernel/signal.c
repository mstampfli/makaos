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

    // SIGKILL is unblockable — forcibly clear the blocked bit.
    if (sig == SIGKILL)
        t->sigstate.blocked &= ~(1u << (uint32_t)(sig - 1));

    // Enqueue into ring buffer.  If full, drop (same as classic bitmask overflow).
    sigstate_t* ss = &t->sigstate;
    uint8_t next_tail = (ss->tail + 1) & (SIG_QUEUE_SIZE - 1);
    if (next_tail != ss->head) {   // not full
        ss->queue[ss->tail] = (uint8_t)sig;
        ss->tail = next_tail;
    }

    // Wake the task if it's sleeping so it processes the signal promptly.
    if (t->state == TASK_SLEEPING)
        sched_wake(t);
}

// ── signal_send_group ─────────────────────────────────────────────────────
// Sends `sig` to every task whose tgid matches.
// Walks the run queue and also checks g_current.
// NOTE: this only reaches tasks in the scheduler's queue + current task.
// A task that is TASK_DEAD is skipped.
typedef struct { uint32_t tgid; int sig; } sig_group_arg_t;

static void sig_group_cb(task_t* t, void* data) {
    sig_group_arg_t* a = (sig_group_arg_t*)data;
    if (t->tgid == a->tgid && t != g_current && t->state != TASK_DEAD)
        signal_send(t, a->sig);
}

void signal_send_group(uint32_t tgid, int sig) {
    if (g_current && g_current->tgid == tgid)
        signal_send(g_current, sig);
    sig_group_arg_t a = {tgid, sig};
    sched_for_each(sig_group_cb, &a);
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

    // Dequeue the first unblocked signal from the ring buffer.
    int sig = 0;
    uint8_t scan = ss->head;
    while (scan != ss->tail) {
        uint8_t candidate = ss->queue[scan];
        uint32_t bit = 1u << (uint32_t)(candidate - 1);
        if (!(ss->blocked & bit)) {
            sig = (int)candidate;
            // Remove this entry by shifting the head past it.
            // If it's not at head, compact the ring to close the gap.
            if (scan == ss->head) {
                ss->head = (ss->head + 1) & (SIG_QUEUE_SIZE - 1);
            } else {
                // Shift entries down to close the gap at `scan`.
                uint8_t i = scan;
                while (i != ss->tail) {
                    uint8_t next = (i + 1) & (SIG_QUEUE_SIZE - 1);
                    ss->queue[i] = ss->queue[next];
                    i = next;
                }
                ss->tail = (ss->tail - 1) & (SIG_QUEUE_SIZE - 1);
            }
            break;
        }
        scan = (scan + 1) & (SIG_QUEUE_SIZE - 1);
    }
    if (!sig) return;

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
