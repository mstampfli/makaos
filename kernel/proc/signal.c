#include "signal.h"
#include "process.h"
#include "sched.h"
#include "common.h"
#include "fb.h"

// ── Globals from syscall.c ────────────────────────────────────────────────
// These are set here to redirect the syscall return path to a signal handler.
extern uint64_t g_syscall_user_rsp;
extern uint64_t g_syscall_user_rip;
extern uint64_t g_syscall_user_rflags;
extern uint64_t g_syscall_user_rbp;
extern uint64_t g_syscall_user_rbx;
extern uint64_t g_syscall_user_r12;
extern uint64_t g_syscall_user_r13;
extern uint64_t g_syscall_user_r14;
extern uint64_t g_syscall_user_r15;
// Set to 1 (handler entry) or 2 (sigreturn restore) by signal/syscall code.
extern uint8_t  g_signal_deliver;
// rdi value to pass to the signal handler (the signal number).
extern uint64_t g_signal_rdi;
// Set to 1 by syscall_dispatch while calling signal_deliver_pending.
// Allows signal_deliver_pending to know it's safe to set up user frames.
extern uint8_t  g_signal_in_syscall;

// ── signal_send ───────────────────────────────────────────────────────────
// Atomically set the pending bit.  Cross-CPU senders never race with the
// receiver's deliver path because bit set/clear are LOCK-prefixed under SMP.
void signal_send(task_t* t, int sig) {
    if (!t) return;
    if (sig < 1 || sig >= NSIG) return;

    uint32_t bit = 1u << (uint32_t)(sig - 1);

    // SIGKILL is unblockable — forcibly clear the blocked bit.
    if (sig == SIGKILL)
        t->sigstate.blocked &= ~bit;

    // Set the pending bit atomically.  Coalesces repeat sends (classic POSIX).
    atomic_or(&t->sigstate.pending, bit);

    // Wake the task if sleeping.
    if (t->state == TASK_SLEEPING)
        sched_wake(t);
}

// ── signal_send_group ─────────────────────────────────────────────────────
// Walk the tgid hash bucket — O(threads in process), not O(total tasks).
// The walker holds the tgid table lock for the duration so concurrent
// task_idx_insert/remove cannot tear the list we're iterating.

static void sig_group_visit(task_t* t, void* data) {
    int sig = *(int*)data;
    if (t->state == TASK_DEAD || t->state == TASK_ZOMBIE) return;
    signal_send(t, sig);
}

void signal_send_group(uint32_t tgid, int sig) {
    task_idx_tgid_walk(tgid, sig_group_visit, &sig);
}

// ── signal_send_pgrp ─────────────────────────────────────────────────────
// Walk the pgid hash bucket — O(procs in pgrp), not O(total tasks).
// Same locking invariant as signal_send_group.
void signal_send_pgrp(uint32_t pgid, int sig) {
    task_idx_pgid_walk(pgid, sig_group_visit, &sig);
}

// ── signal_setup_frame ────────────────────────────────────────────────────
// Build a sigframe_t on the user's stack and redirect the syscall return to
// the handler.  Only call this when g_signal_in_syscall == 1.
static void signal_setup_frame(int sig, k_sigaction_t* ka) {
    uint64_t user_rsp = g_syscall_user_rsp;

    // Place sigframe below current RSP, 16-byte aligned.
    uint64_t frame_base = (user_rsp - sizeof(sigframe_t)) & ~(uint64_t)0xF;
    sigframe_t* frame = (sigframe_t*)frame_base;

    frame->rip    = g_syscall_user_rip;
    frame->rsp    = user_rsp;
    frame->rflags = g_syscall_user_rflags;
    frame->rbp    = g_syscall_user_rbp;
    frame->rbx    = g_syscall_user_rbx;
    frame->r12    = g_syscall_user_r12;
    frame->r13    = g_syscall_user_r13;
    frame->r14    = g_syscall_user_r14;
    frame->r15    = g_syscall_user_r15;
    frame->blocked = g_current->sigstate.blocked;
    frame->_pad   = 0;

    // Remember where the frame is for sys_sigreturn.
    g_current->sigstate.sigframe_rsp = frame_base;

    // Block the signal itself + sa_mask during handler execution.
    g_current->sigstate.blocked |= (1u << (uint32_t)(sig - 1));
    g_current->sigstate.blocked |= ka->sa_mask;

    // Push restorer address as the "return address" for the handler.
    uint64_t new_rsp = frame_base - 8;
    *(uint64_t*)new_rsp = ka->sa_restorer;

    // Redirect syscall return to the handler.
    g_syscall_user_rip    = ka->sa_handler;
    g_syscall_user_rsp    = new_rsp;
    g_syscall_user_rflags = 0x202;  // IF=1, reserved

    // Tell syscall_entry.asm to load rdi=signum before sysretq.
    g_signal_rdi     = (uint64_t)(uint32_t)sig;
    g_signal_deliver = 1;

}

// ── signal_deliver_pending ────────────────────────────────────────────────
void signal_deliver_pending(void) {
    if (!g_current || g_current->state == TASK_DEAD) return;

    sigstate_t* ss = &g_current->sigstate;

    // Pick the lowest-numbered pending-and-not-blocked signal.  Classic
    // POSIX priority: lower signal numbers deliver first (so a pending
    // SIGKILL wins over SIGCHLD).
    uint32_t eff = ss->pending & ~ss->blocked;
    if (!eff) return;
    int bit = __builtin_ctz(eff);
    int sig = bit + 1;
    // Clear the pending bit atomically before dispatching.
    atomic_clear_bit(&ss->pending, (unsigned)bit);

    k_sigaction_t* ka = &ss->handlers[sig < NSIG ? sig : 0];
    uint64_t handler = (sig < NSIG) ? ka->sa_handler : (uint64_t)SIG_DFL;

    // SIG_IGN: silently discard.
    if (handler == (uint64_t)SIG_IGN) return;

    // Signals whose POSIX default action is "ignore" — swallow them even when
    // the process never installed a handler. Terminating on SIGWINCH was
    // killing every client that did TIOCSWINSZ on its pty.
    if (handler == (uint64_t)SIG_DFL &&
        (sig == SIGWINCH || sig == SIGCHLD))
        return;

    // User handler: only deliverable on the syscall return path.
    if (handler != (uint64_t)SIG_DFL && g_signal_in_syscall &&
        !(g_current->flags & TASK_FLAG_KTHREAD)) {
        signal_setup_frame(sig, ka);
        return;
    }

    // SIG_DFL (or non-syscall path): print diagnostic for fatal hw signals,
    // then terminate.
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL) {
        static const char* names[] = {
            [SIGSEGV] = "SIGSEGV",
            [SIGBUS]  = "SIGBUS ",
            [SIGFPE]  = "SIGFPE ",
            [SIGILL]  = "SIGILL ",
        };
        const char* name = (sig < NSIG && names[sig]) ? names[sig] : "SIG???";
        uint32_t saved_fg = g_fb_fg;
        g_fb_fg = FB_LRED;
        const char* prefix = "PF-KILL ";
        for (int i = 0; prefix[i]; i++) fb_term_putc(prefix[i]);
        for (int i = 0; name[i]; i++) fb_term_putc(name[i]);
        fb_term_putc('\n');
        g_fb_fg = saved_fg;
    }

    // Reparent any children to init before we vanish.
    extern task_t* g_init_task;
    task_t* child = g_current->children;
    while (child) {
        task_t* next_child = child->child_next;
        if (g_init_task && g_init_task != g_current) {
            child->ppid = g_init_task->pid;
            task_child_add(g_init_task, child);
        } else {
            child->ppid = 0;
            child->child_next = NULL;
        }
        child = next_child;
    }
    g_current->children = NULL;

    // Drop the fd table now so peers see EOF immediately (matching sys_exit).
    if (g_current->files_shared) {
        task_files_release(g_current->files_shared);
        g_current->files_shared = NULL;
    }

    // Zombie instead of TASK_DEAD so the parent can reap via waitpid.
    g_current->exit_code = -(int32_t)sig;
    g_current->state = TASK_ZOMBIE;
    sched_add_zombie(g_current);

    // Wake parent if it's sleeping in sys_wait.
    task_t* parent = sched_find_pid(g_current->ppid);
    if (parent && parent->state == TASK_SLEEPING)
        sched_wake(parent);

    sched_yield();
    for (;;) __asm__ volatile("hlt");
}
