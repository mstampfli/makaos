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
void signal_send(task_t* t, int sig) {
    if (!t) return;
    if (sig < 1 || sig >= NSIG) return;

    // SIGKILL is unblockable — forcibly clear the blocked bit.
    if (sig == SIGKILL)
        t->sigstate.blocked &= ~(1u << (uint32_t)(sig - 1));

    // Enqueue into ring buffer.  If full, drop.
    sigstate_t* ss = &t->sigstate;
    uint8_t next_tail = (ss->tail + 1) & (SIG_QUEUE_SIZE - 1);
    if (next_tail != ss->head) {
        ss->queue[ss->tail] = (uint8_t)sig;
        ss->tail = next_tail;
    }

    // Wake the task if sleeping.
    if (t->state == TASK_SLEEPING)
        sched_wake(t);
}

// ── signal_send_group ─────────────────────────────────────────────────────
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

// ── signal_send_pgrp ─────────────────────────────────────────────────────
// Send signal to every task in the given process group.
typedef struct { uint32_t pgid; int sig; } sig_pgrp_arg_t;

static void sig_pgrp_cb(task_t* t, void* data) {
    sig_pgrp_arg_t* a = (sig_pgrp_arg_t*)data;
    if (t->pgid == a->pgid && t->state != TASK_DEAD)
        signal_send(t, a->sig);
}

void signal_send_pgrp(uint32_t pgid, int sig) {
    if (g_current && g_current->pgid == pgid)
        signal_send(g_current, sig);
    sig_pgrp_arg_t a = {pgid, sig};
    sched_for_each(sig_pgrp_cb, &a);
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

    // Dequeue the first unblocked signal.
    int sig = 0;
    uint8_t scan = ss->head;
    while (scan != ss->tail) {
        uint8_t candidate = ss->queue[scan];
        uint32_t bit = 1u << (uint32_t)(candidate - 1);
        if (!(ss->blocked & bit)) {
            sig = (int)candidate;
            if (scan == ss->head) {
                ss->head = (ss->head + 1) & (SIG_QUEUE_SIZE - 1);
            } else {
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

    k_sigaction_t* ka = &ss->handlers[sig < NSIG ? sig : 0];
    uint64_t handler = (sig < NSIG) ? ka->sa_handler : (uint64_t)SIG_DFL;

    // SIG_IGN: silently discard.
    if (handler == (uint64_t)SIG_IGN) return;

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
