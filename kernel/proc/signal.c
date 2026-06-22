#include "signal.h"
#include "process.h"
#include "sched.h"
#include "cpu.h"
#include "common.h"
#include "fb.h"

// ── Saved user context access ────────────────────────────────────────────
// The authoritative copy of a syscall's saved user registers lives on
// the PER-TASK kernel stack (SYSCALL_KFRAME in process.h), pushed by
// syscall_entry.asm before interrupts are re-enabled.  signal frames
// are built from — and the syscall return redirected through — that
// frame.  The per-CPU cpu_t scratch slots must NOT be read here: they
// go stale the moment the task sleeps mid-syscall and resumes on
// another CPU (observed: bash received a foot pthread-stack RSP for
// its SIGWINCH frame; the write to the unmapped address panicked the
// kernel inside signal_setup_frame).
//
// signal_in_syscall stays per-CPU: it is set and consumed around the
// signal_deliver_pending call inside one dispatch invocation on one
// CPU, with no sleep in between.
#define g_signal_in_syscall   (this_cpu()->signal_in_syscall)

// ── signal_send ───────────────────────────────────────────────────────────
// Atomically set the pending bit.  Cross-CPU senders never race with the
// receiver's deliver path because bit set/clear are LOCK-prefixed under SMP.
void signal_send(task_t* t, int sig) {
    if (!t) return;
    if (sig < 1 || sig >= NSIG) return;

    /* Trace only the fatal/terminating signals to keep volume sane —
     * SIGCHLD + SIGWINCH fly constantly. */
    if (sig != SIGCHLD && sig != SIGWINCH) {
        extern void kprintf(const char*, ...);
        kprintf("[signal] send: sig=%d → pid=%u comm=\"%s\" "
                "(sender pid=%u comm=\"%s\")\n",
                sig, (unsigned)t->pid, t->comm,
                g_current ? (unsigned)g_current->pid : 0,
                g_current ? g_current->comm : "(none)");
    }

    uint32_t bit = 1u << (uint32_t)(sig - 1);

    // SIGKILL is unblockable — forcibly clear the blocked bit.
    if (sig == SIGKILL)
        t->sigstate.blocked &= ~bit;

    // Set the pending bit atomically.  Coalesces repeat sends (classic POSIX).
    atomic_or(&t->sigstate.pending, bit);

    // Wake any signalfd subscribed to this signal on the target task.
    // signalfd_notify walks t->signalfd_head; no-op if the list is empty.
    extern void signalfd_notify(task_t* t, int sig);
    signalfd_notify(t, sig);

    // Always go through sched_wake.  The previous "if (t->state ==
    // TASK_SLEEPING) sched_wake(t)" optimisation is a lost-wakeup bug
    // under SMP: between the sender's racy state read and the sleeper
    // actually parking itself in the sleep list, the sleeper's state
    // transitions RUNNING→SLEEPING, and the sender — having observed
    // RUNNING — skips the wake entirely.  The sleeper then enters
    // sched_sleep, sees wake_pending==0 (sched_wake was never called),
    // and sleeps forever.
    //
    // sched_wake handles the state check under the target's rq_lock
    // and falls back to wake_pending for any racy not-yet-SLEEPING
    // case, so it is always safe and cheap to call.
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
//
// ABI: x86-64 System V reserves a 128-byte "red zone" below the current
// user rsp for leaf function scratch (locals, register spills).  When
// the kernel delivers a signal, it MUST NOT clobber this area — POSIX
// applies the same rule, and compilers emit code that stores important
// data (including struct sigaction locals and saved register spills
// from callers) in that region.  Skipping the red zone is a hard
// requirement, not an optimisation.
//
// Pre-fix: this routine placed the sigframe at (user_rsp -
// sizeof(sigframe_t)), writing directly into the red zone of both the
// current leaf and any pending restore state in the caller's spill
// slots.  The observed symptom was bash crashing at RIP=0 after a
// `ret` from set_signal_handler, because the caller's red-zone spill
// of its own return address (or the callee-saved register holding a
// function pointer) was being overwritten with sigframe bytes during
// SIGCHLD delivery on the sys_sigaction return path.  See the PF-KILL
// dump in serial.txt: bash at 0x47b27f (set_signal_handler, right
// after the sigaction syscall), RIP=0 ifetch fault, every gpr zero
// except RCX which holds the post-syscall instruction pointer.
// Boundary check: `addr` must be a canonical user address AND the
// `len`-byte range starting there must stay in the low canonical half
// [0, 2^47).  The non-canonical gap between 2^47 and HHDM_OFFSET
// (0xFFFF800000000000) must also be rejected — any address there
// #GPs the moment the CPU tries to ring-transition back to it via
// iretq.  Using HHDM_OFFSET as the ceiling here would silently let
// non-canonical pointers through and is the bug that produced the
// Ctrl+C iretq #GP on makaterm.
#define USER_ADDR_CEIL (1ULL << 47)

static inline int sig_user_range_ok(uint64_t addr, uint64_t len) {
    if (!addr) return 0;
    if (addr >= USER_ADDR_CEIL) return 0;
    if (addr + len < addr) return 0;               // wrap
    if (addr + len > USER_ADDR_CEIL) return 0;
    return 1;
}

static void signal_setup_frame(int sig, k_sigaction_t* ka) {
    // Per-task saved user context — survives mid-syscall CPU migration.
    syscall_kframe_t* kf = SYSCALL_KFRAME(g_current);
    uint64_t user_rsp = kf->rsp;

    // Skip the 128-byte red zone, then place the sigframe below it,
    // 16-byte aligned as required for stack-passed arguments.
    uint64_t frame_base = (user_rsp - 128 - sizeof(sigframe_t))
                            & ~(uint64_t)0xF;

    // Validate: the whole [frame_base-8, user_rsp) window must live in
    // the user half.  If user_rsp is garbage (e.g. a buggy handler
    // clobbered it before raising another signal), writing the sigframe
    // would clobber kernel memory.  Kill the task with SIGSEGV instead.
    if (!sig_user_range_ok(frame_base - 8, sizeof(sigframe_t) + 8)) {
        // Force-queue SIGKILL for the next delivery cycle.  We can't
        // kill the task inline from here — the terminate block below
        // is the only safe place (drops fds, reparents children,
        // zombifies), and it wants a fall-through, not a nested call.
        // Leaving the signal pending-with-no-handler means the next
        // signal_deliver_pending takes the SIG_DFL-terminate path.
        extern void kprintf(const char*, ...);
        kprintf("[signal] setup_frame RANGE kill: comm=\"%s\" sig=%d "
                "kf_rsp=%p frame=%p\n",
                g_current->comm, sig, (void*)user_rsp, (void*)frame_base);
        atomic_or(&g_current->sigstate.pending,
                  1u << (uint32_t)(SIGKILL - 1));
        g_current->sigstate.handlers[SIGKILL].sa_handler = (uint64_t)SIG_DFL;
        return;
    }

    /* Handler / restorer validation.
     *
     * Both must be canonical user pointers (< USER_ADDR_CEIL = 2^47).
     * Additionally, sa_handler must be non-zero — a zero handler means
     * "SIG_DFL" by POSIX convention, and the caller guards against that
     * before reaching here, but if the guard is ever bypassed we must
     * NEVER write RIP=0 into g_syscall_user_rip (§195).
     *
     * sa_restorer MUST also be non-zero and valid: when the handler
     * returns it `ret`s to the restorer, which calls sigreturn(2) to
     * unwind.  A zero sa_restorer means the handler's `ret` pops 0 off
     * the user stack → user #PF at RIP=0 (observed: Ctrl+C / SIGINT
     * against dwl, comm=dwl, CR2=0, RIP=0, RCX inside libc syscall6).
     *
     * A syscall that registers a signal handler without a working
     * restorer is a userland bug, but the kernel must not crash over
     * it — force SIGKILL and take the SIG_DFL terminate path instead.
     */
    if (ka->sa_handler == 0 || ka->sa_handler >= USER_ADDR_CEIL ||
        ka->sa_restorer == 0 || ka->sa_restorer >= USER_ADDR_CEIL) {
        // Force-queue SIGKILL for the next delivery cycle.  We can't
        // kill the task inline from here — the terminate block below
        // is the only safe place (drops fds, reparents children,
        // zombifies), and it wants a fall-through, not a nested call.
        // Leaving the signal pending-with-no-handler means the next
        // signal_deliver_pending takes the SIG_DFL-terminate path.
        extern void kprintf(const char*, ...);
        kprintf("[signal] setup_frame HANDLER kill: comm=\"%s\" sig=%d "
                "handler=%p restorer=%p\n",
                g_current->comm, sig,
                (void*)ka->sa_handler, (void*)ka->sa_restorer);
        atomic_or(&g_current->sigstate.pending,
                  1u << (uint32_t)(SIGKILL - 1));
        g_current->sigstate.handlers[SIGKILL].sa_handler = (uint64_t)SIG_DFL;
        return;
    }

    // Defense in depth: the canonical-range check above cannot tell a
    // VALID-but-unmapped address from a mapped one — and a kernel-mode
    // write to a user address with NO covering VMA is an unrecoverable
    // #PF (panic), not a demand-page.  Require the whole frame window
    // to be VMA-covered; demand paging handles not-yet-present pages.
    {
        extern vma_t* mm_vma_find(mm_t*, virt_addr_t);
        mm_t* mm = g_current->mm_shared->mm;
        if (!mm || !mm_vma_find(mm, frame_base - 8) ||
            !mm_vma_find(mm, frame_base + sizeof(sigframe_t) - 1)) {
            extern void kprintf(const char*, ...);
            kprintf("[signal] setup_frame VMA kill: comm=\"%s\" sig=%d "
                    "kf_rsp=%p frame=%p\n",
                    g_current->comm, sig, (void*)user_rsp, (void*)frame_base);
            atomic_or(&g_current->sigstate.pending,
                      1u << (uint32_t)(SIGKILL - 1));
            g_current->sigstate.handlers[SIGKILL].sa_handler = (uint64_t)SIG_DFL;
            return;
        }
    }

    sigframe_t* frame = (sigframe_t*)frame_base;

    frame->rip    = kf->rip;
    frame->rsp    = user_rsp;
    frame->rflags = kf->rflags;
    frame->rbp    = kf->rbp;
    frame->rbx    = kf->rbx;
    frame->r12    = kf->r12;
    frame->r13    = kf->r13;
    frame->r14    = kf->r14;
    frame->r15    = kf->r15;
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

    // Redirect the syscall return to the handler by mutating the saved
    // user context in place — the exit path's normal pop sequence
    // consumes these slots, so no per-CPU flag and no special asm
    // branch are involved.  rdi is the handler's signum argument.
    kf->rip    = ka->sa_handler;
    kf->rsp    = new_rsp;
    kf->rflags = 0x202;  // IF=1, reserved
    kf->rdi    = (uint64_t)(uint32_t)sig;
}

// ── signal_deliver_pending ────────────────────────────────────────────────
void signal_deliver_pending(int may_setup_frame) {
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
    // signal_setup_frame writes a sigframe on the user stack and
    // redirects sysretq / iretq to the handler entry — it can only
    // work when we're about to return to user space via one of those
    // instructions, which the kernel signals by setting
    // g_signal_in_syscall=1.
    // Frame setup is ONLY valid when the caller guarantees we are on
    // the current task's own syscall-return path (it owns its
    // SYSCALL_KFRAME and is about to sysret/iret through it).  This is
    // now an explicit per-call argument instead of a per-CPU flag: the
    // old g_signal_in_syscall could be stale-1 when an IRQ preempted a
    // syscall return and do_switch then built a sigframe for the
    // WRONG (incoming) task at its stale kframe rsp — corrupting that
    // task's user stack (a return address overwritten with the signal
    // number / register bytes).  do_switch and the fault/exception
    // paths pass 0 → defer; only the syscall-return caller passes 1.
    if (handler != (uint64_t)SIG_DFL && may_setup_frame &&
        !(g_current->flags & TASK_FLAG_KTHREAD)) {
        signal_setup_frame(sig, ka);
        return;
    }

    // Non-syscall path (e.g. called from do_switch after a
    // context_switch wake) — the task has a custom handler but
    // we're not on a path that can set up a user frame right now.
    // DEFER: re-set the pending bit so the next signal_deliver_pending
    // call (which WILL be on a syscall return path, when the task
    // next enters the kernel and exits it) can install the frame.
    // Pre-fix: the code fell through to the fatal-terminate block
    // below, silently killing any task that woke up via context
    // switch with a pending custom-handler signal.  Observed: during
    // the pty/makaterm freeze, bash PF-crashed (SIGSEGV to self),
    // which sent SIGCHLD to makaterm.  makaterm had a custom SIGCHLD
    // handler and was parked in epoll_wait.  do_switch woke it,
    // called signal_deliver_pending with g_signal_in_syscall=0, the
    // custom-handler gate failed, and the kernel zombified makaterm
    // via the fatal-non-hw fall-through.  Fix: re-queue the signal
    // and return so the task continues running; it will re-enter
    // signal_deliver_pending on its next syscall-return where the
    // gate succeeds.
    if (handler != (uint64_t)SIG_DFL &&
        !(g_current->flags & TASK_FLAG_KTHREAD)) {
        atomic_or(&ss->pending, 1u << (uint32_t)(sig - 1));
        return;
    }

    // SIG_DFL (and non-ignored) or kernel thread: print diagnostic
    // for fatal hw signals, then terminate.
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

        /* Also echo to serial so post-mortem log analysis can tell
         * WHICH task caught the fatal signal.  The fb_term print
         * above doesn't land in serial.txt. */
        extern void kprintf(const char*, ...);
        kprintf("[signal] terminate: pid=%u comm=\"%s\" sig=%s\n",
                (unsigned)g_current->pid, g_current->comm, name);
    }
    /* Log non-fatal SIG_DFL terminations too (SIGTERM, SIGINT, SIGPIPE,
     * etc.) — we want to see who called sys_kill / whose tty raised it. */
    else if (sig != SIGCHLD && sig != SIGWINCH) {
        extern void kprintf(const char*, ...);
        kprintf("[signal] terminate: pid=%u comm=\"%s\" sig=%d (SIG_DFL)\n",
                (unsigned)g_current->pid, g_current->comm, sig);
    }

    // Reparent any children to init before we vanish.  Batched CAS
    // splice — see task_children_reparent in sched.c for the
    // memory-ordering contract.
    extern task_t* g_init_task;
    if (g_init_task && g_init_task != g_current) {
        task_children_reparent(g_current, g_init_task);
    } else {
        g_current->children = NULL;
    }

    // Drop the fd table now so peers see EOF immediately (matching sys_exit).
    if (g_current->files_shared) {
        extern void kprintf(const char*, ...);
        {
            extern void task_notify_cleartid(void);
            task_notify_cleartid();
        }
        kprintf("[signal] releasing files of pid=%u comm=\"%s\" refs=%u\n",
                (unsigned)g_current->pid, g_current->comm,
                (unsigned)g_current->files_shared->refs);
        task_files_release(g_current->files_shared);
        g_current->files_shared = NULL;
    } else {
        extern void kprintf(const char*, ...);
        kprintf("[signal] pid=%u comm=\"%s\" has NO files_shared (leak?)\n",
                (unsigned)g_current->pid, g_current->comm);
    }

    // Zombie instead of TASK_DEAD so the parent can reap via waitpid.
    g_current->exit_code = -(int32_t)sig;
    g_current->state = TASK_ZOMBIE;
    sched_add_zombie(g_current);

    // Notify the parent — exactly the same path sys_exit takes.  This
    // was previously only `sched_wake(parent)` and only when state was
    // SLEEPING, which dropped SIGCHLD entirely AND was a textbook
    // lost-wakeup race.  Mirror sys_exit: signal_send sets the pending
    // bit (so sys_wait's signal-driven retry runs) AND unconditionally
    // calls sched_wake under the target's rq_lock, which handles every
    // RUNNING/SLEEPING/in-flight transition correctly.
    task_t* parent = sched_find_pid(g_current->ppid);
    if (parent && parent->state != TASK_ZOMBIE)
        signal_send(parent, SIGCHLD);

    sched_yield();
    for (;;) __asm__ volatile("hlt");
}
