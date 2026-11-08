#include "signal.h"
#include "idt.h"       // trap_frame_t, interrupt_frame_t, TRAP_FROM_IFRAME (fault delivery)
#include "kprintf.h"   // kprintf_atomic (locked whole-line output for selftest result lines)
#include "process.h"
#include "sched.h"
#include "cpu.h"
#include "common.h"
#include "fb.h"
#include "errno.h"   // ESRCH (signal_send_pid)
#include "uaccess.h" // _access_ok: shared user-range validator (was sig_user_range_ok)
#include "kheap.h"   // kmalloc/kfree (sighand_t lifecycle)

// ── sighand_t lifecycle (shared per-process signal disposition table) ──────
// Refcounted, allocated on the kernel heap.  A thread group shares one; fork
// copies; exit releases.  See signal.h for the sharing contract.
sighand_t* sighand_alloc(void) {
    sighand_t* sh = kmalloc(sizeof(sighand_t));
    if (!sh) return NULL;
    sh->refs = 1;
    __builtin_memset(sh->handlers, 0, sizeof(sh->handlers));   // all SIG_DFL
    return sh;
}

sighand_t* sighand_copy(const sighand_t* src) {
    sighand_t* sh = kmalloc(sizeof(sighand_t));
    if (!sh) return NULL;
    sh->refs = 1;
    if (src) __builtin_memcpy(sh->handlers, src->handlers, sizeof(sh->handlers));
    else     __builtin_memset(sh->handlers, 0, sizeof(sh->handlers));
    return sh;
}

void sighand_release(sighand_t* sh) {
    if (!sh) return;
    if (__atomic_sub_fetch(&sh->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree(sh);
}

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

    // SIGKILL is unblockable — forcibly clear the blocked bit.  This runs on
    // the SENDER's CPU and writes the TARGET's mask, racing the target's own
    // mask RMWs (sigprocmask / signal_setup_frame).  Must be atomic, like the
    // `pending` update below, or a concurrent owner RMW loses this clear.
    if (sig == SIGKILL)
        atomic_and(&t->sigstate.blocked, ~bit);

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

// ── signal_send_pid ──────────────────────────────────────────────────────
// Look up `pid` and deliver `sig`, all inside one rcu_read_lock section.
// sched_find_pid (pid_ht_find) drops its OWN reader section before returning,
// so a bare `t = sched_find_pid(pid); signal_send(t,...)` derefs the task
// outside RCU -> a concurrent exit+task_destroy (which RCU-defers the free via
// task_free_rcu) can free `t` in the window -> use-after-free.  Holding the
// reader section across the delivery keeps `t` alive (the grace period cannot
// complete while we are inside it).  signal_send is rcu/lock-safe (atomic bit
// + sched_wake -> rq_lock, no sleep).  Zombies are skipped (semantically dead).
// Returns 0 on delivery, -ESRCH if no such live task.
int signal_send_pid(uint32_t pid, int sig) {
    rcu_read_lock();
    task_t* t = sched_find_pid(pid);
    int delivered = (t && t->state != TASK_ZOMBIE);
    if (delivered)
        signal_send(t, sig);
    rcu_read_unlock();
    return delivered ? 0 : -ESRCH;
}

// ── kill() permission (POSIX) ─────────────────────────────────────────────
// A user process `sender` may signal `target` iff sender is root (euid 0) or
// sender's real/effective uid equals the target's real or saved uid.  Self is
// always allowed.  Used ONLY on the user kill() path; kernel-internal signals
// (SIGCHLD, tty job control, WINCH, exit_group) call the unchecked helpers.
int signal_may(const task_t* sender, const task_t* target) {
    if (!sender || !target) return 0;
    if (sender == target) return 1;
    if (sender->cred.euid == 0) return 1;
    uint32_t se = sender->cred.euid, sr = sender->cred.ruid;
    uint32_t tr = target->cred.ruid, ts = target->cred.suid;
    return se == tr || se == ts || sr == tr || sr == ts;
}

// kill(pid>0), authorized: -ESRCH if no live task, -EPERM if not permitted.
int signal_send_pid_user(task_t* sender, uint32_t pid, int sig) {
    rcu_read_lock();
    task_t* t = sched_find_pid(pid);
    int found = (t && t->state != TASK_ZOMBIE);
    int perm  = found && signal_may(sender, t);
    if (perm) signal_send(t, sig);
    rcu_read_unlock();
    return !found ? -ESRCH : (perm ? 0 : -EPERM);
}

typedef struct { int sig; task_t* sender; } sig_perm_ctx_t;
static void sig_group_visit_perm(task_t* t, void* data) {
    sig_perm_ctx_t* c = (sig_perm_ctx_t*)data;
    if (t->state == TASK_DEAD || t->state == TASK_ZOMBIE) return;
    if (!signal_may(c->sender, t)) return;   // silently skip un-signalable members
    signal_send(t, c->sig);
}
// kill(0) / kill(-pgid), authorized: signal only members the sender may.
void signal_send_pgrp_user(task_t* sender, uint32_t pgid, int sig) {
    sig_perm_ctx_t c = { sig, sender };
    task_idx_pgid_walk(pgid, sig_group_visit_perm, &c);
}

// Pure permission-decision unit test for signal_may.
void signal_perm_selftest(void) {
    extern void kprintf(const char*, ...);
    task_t root = {0}, alice = {0}, alice2 = {0}, bob = {0};
    root.cred.euid = 0; root.cred.ruid = 0;
    alice.cred.ruid  = alice.cred.euid  = alice.cred.suid  = 1000;
    alice2.cred.ruid = alice2.cred.euid = alice2.cred.suid = 1000;
    bob.cred.ruid    = bob.cred.euid    = bob.cred.suid    = 1001;
    int f = 0;
    if (!signal_may(&root, &bob))     f++;   // root -> anyone
    if (!signal_may(&alice, &alice))  f++;   // self
    if (!signal_may(&alice, &alice2)) f++;   // same uid
    if ( signal_may(&alice, &bob))    f++;   // cross uid -> denied
    if ( signal_may(&alice, &root))   f++;   // non-root cannot signal root
    kprintf_atomic(f ? "[signal_perm] SELF-TEST FAILED\n"
              : "[signal_perm] SELF-TEST PASSED (kill authz: root-any/self/same-uid; cross-uid denied)\n");
}

// Deterministic guard for the rcu-safe lookup helper.  Sending to a pid that
// cannot exist takes the not-found path (no signal_send, no side effects) and
// must return -ESRCH; reaching the next line at all proves the rcu_read_lock
// section is balanced (a leak would leave preempt disabled and wedge the CPU).
void signal_send_pid_selftest(void) {
    extern void kprintf(const char*, ...);
    int r = signal_send_pid(0xFFFFFFFEu, 15 /* SIGTERM; unused on this path */);
    kprintf(r == -ESRCH
            ? "[signal_send_pid] SELF-TEST PASSED (unknown pid -> ESRCH, rcu balanced)\n"
            : "[signal_send_pid] SELF-TEST FAILED r=%d\n", r);
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
// The sigframe window must stay in the low canonical user half [0, 2^47):
// the non-canonical gap up to HHDM_OFFSET must be rejected too, else writing
// there #GPs the kernel at the iretq ring-transition (the Ctrl+C makaterm
// crash).  That is exactly _access_ok (uaccess.h) -- was a local
// sig_user_range_ok that hand-reimplemented it; consolidated so the signal
// path can never drift weaker than the syscall path.

// Force-queue SIGKILL and take the SIG_DFL terminate path (used when the user
// stack for a frame is unusable).  Mirrors the inline kills in the frame path.
static void signal_force_kill(int sig, const char* why, uint64_t rsp, uint64_t fb) {
    extern void kprintf(const char*, ...);
    kprintf("[signal] %s kill: comm=\"%s\" sig=%d kf_rsp=%p frame=%p\n",
            why, g_current->comm, sig, (void*)rsp, (void*)fb);
    atomic_or(&g_current->sigstate.pending, 1u << (uint32_t)(SIGKILL - 1));
    g_current->sighand->handlers[SIGKILL].sa_handler = (uint64_t)SIG_DFL;
}

// Choose the stack to place a signal frame on.  If the handler asked for
// SA_ONSTACK and a per-thread alternate stack is set AND we are NOT already
// executing on it, deliver on the TOP of the alt stack (it grows down) -- this
// is how a SIGSEGV from a stack overflow is caught, since the interrupted stack
// is then unusable.  Otherwise deliver on the interrupted rsp.  The SAVED
// context (gregs[REG_RSP] / sigframe.rsp) always keeps the interrupted rsp, so
// sigreturn resumes on the original stack.
static uint64_t sig_stack_base(const k_sigaction_t* ka, uint64_t interrupted_rsp) {
    if (ka->sa_flags & SA_ONSTACK) {
        const sigstate_t* ss = &g_current->sigstate;
        uint64_t sp = (uint64_t)ss->altstack_sp;
        uint64_t sz = ss->altstack_size;
        if (sp && sz && !(interrupted_rsp >= sp && interrupted_rsp < sp + sz))
            return sp + sz;   // top of the alt stack
    }
    return interrupted_rsp;
}

// SA_SIGINFO delivery: build a Linux-style rt_sigframe { pretcode, ucontext,
// siginfo } on the user stack from the interrupted context *src, populate the
// ucontext gregs + si_addr from the fault, and produce the handler-entry
// redirect in *redir (rdi=sig, rsi=&info, rdx=&uc).  sys_sigreturn restores
// FROM uc.uc_mcontext (honouring handler edits to gregs[REG_RIP] -- the JVM
// resume-past-fault mechanism).  Source-neutral: *src is filled from either a
// syscall kframe or an exception trap frame, so both delivery paths share this
// one builder and cannot drift.  Returns 0 on success, -1 if it force-killed
// the task (unusable user stack); the caller then takes the terminate path.
static int build_rt_frame(int sig, k_sigaction_t* ka,
                          const sig_uctx_t* src, sig_redirect_t* redir) {
    uint64_t user_rsp = src->rsp;
    // Place the frame on the alt stack when SA_ONSTACK is set; the saved
    // gregs[KREG_RSP] below stays user_rsp so sigreturn resumes the old stack.
    uint64_t stack_base = sig_stack_base(ka, user_rsp);

    // frame_base ≡ 8 (mod 16): 16-align the floor below the red zone, then -8,
    // so at handler entry rsp%16==8 (SysV) AND the embedded fpstate at
    // frame_base+312 is 16-aligned for fxsave/fxrstor.
    uint64_t frame_base =
        ((stack_base - 128 - sizeof(k_rt_sigframe_t)) & ~(uint64_t)0xF) - 8;

    if (!_access_ok(frame_base - 8, sizeof(k_rt_sigframe_t) + 8)) {
        signal_force_kill(sig, "rt RANGE", user_rsp, frame_base); return -1;
    }
    if (ka->sa_handler == 0 || ka->sa_handler >= USER_ADDR_CEIL ||
        ka->sa_restorer == 0 || ka->sa_restorer >= USER_ADDR_CEIL) {
        signal_force_kill(sig, "rt HANDLER", user_rsp, frame_base); return -1;
    }
    // Build the frame in a kernel-local buffer, then publish it to the user
    // stack via copy_to_user.  copy_to_user is fault-safe (fault-fixup table):
    // a bad or racing user stack page -- e.g. a sibling thread munmap'd it
    // during process teardown -- returns -EFAULT here instead of taking an
    // unrecoverable kernel #PF.  A dying or hostile process can therefore never
    // panic the kernel by making its signal frame land on an unmapped page.
    // `rf` is used ONLY for address arithmetic (the user-visible addresses of
    // the embedded siginfo/ucontext/fpstate); it is never dereferenced.
    k_rt_sigframe_t* rf = (k_rt_sigframe_t*)frame_base;
    k_rt_sigframe_t kf;
    __builtin_memset(&kf, 0, sizeof(kf));
    uint64_t fault = g_current->sigstate.fault_addr;

    // siginfo
    kf.info.si_signo  = sig;
    kf.info.si_errno  = 0;
    kf.info.si_code   = g_current->sigstate.fault_si_code;  // fault code, or 0 (SI_USER)
    kf.info.si_pid    = 0;
    kf.info.si_uid    = 0;
    kf.info.si_status = 0;
    kf.info.si_addr   = (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE ||
                         sig == SIGILL || sig == SIGTRAP)
                        ? (void*)fault : (void*)0;
    kf.info.si_value  = 0;

    // ucontext
    kf.uc.uc_flags = 0;
    kf.uc.uc_link  = 0;
    kf.uc.uc_stack.ss_sp = 0; kf.uc.uc_stack.ss_flags = 2 /*SS_DISABLE*/;
    kf.uc.uc_stack.ss_size = 0;
    long long* g = kf.uc.uc_mcontext.gregs;
    g[KREG_RIP] = (long long)src->rip;
    g[KREG_RSP] = (long long)user_rsp;
    g[KREG_EFL] = (long long)src->rflags;
    g[KREG_RBP] = (long long)src->rbp;  g[KREG_RBX] = (long long)src->rbx;
    g[KREG_R12] = (long long)src->r12;  g[KREG_R13] = (long long)src->r13;
    g[KREG_R14] = (long long)src->r14;  g[KREG_R15] = (long long)src->r15;
    g[KREG_RDI] = (long long)src->rdi;  g[KREG_RSI] = (long long)src->rsi;
    g[KREG_RDX] = (long long)src->rdx;  g[KREG_R10] = (long long)src->r10;
    g[KREG_R8]  = (long long)src->r8;   g[KREG_R9]  = (long long)src->r9;
    g[KREG_RAX] = (long long)src->rax;
    // rcx/r11 are captured from an exception trap frame; from a syscall kframe
    // they are clobbered by `syscall` and the filler leaves them zero.
    g[KREG_RCX] = (long long)src->rcx; g[KREG_R11] = (long long)src->r11;
    g[KREG_CR2] = (long long)fault;
    g[KREG_CSGSFS] = 0; g[KREG_ERR] = 0; g[KREG_TRAPNO] = 0; g[KREG_OLDMASK] = 0;
    kf.uc.uc_mcontext.fpregs = (void*)&rf->uc.__fpregs_mem;  // USER address
    // k_rt_sigframe_t carries no alignment on __fpregs_mem (it relied on the
    // frame_base ≡ 8 (mod 16) user placement, which a kernel-stack copy does
    // not reproduce).  fxsave requires a 16-byte-aligned target, so save into
    // an aligned scratch buffer, then copy into the (possibly unaligned) field.
    // Entry stubs keep RSP 16-aligned before calling C, so this local is valid.
    _Alignas(16) uint8_t fpu_scratch[512];
    __asm__ volatile("fxsave %0" : "=m"(fpu_scratch));
    __builtin_memcpy(kf.uc.__fpregs_mem, fpu_scratch, sizeof(fpu_scratch));
    kf.uc.uc_sigmask = g_current->sigstate.blocked;

    kf.pretcode = ka->sa_restorer;       // handler's return address -> restorer -> sigreturn

    extern int copy_to_user(void* dst_u, const void* src, uint64_t len);
    if (copy_to_user((void*)frame_base, &kf, sizeof(kf)) != 0) {
        signal_force_kill(sig, "rt COPY", user_rsp, frame_base); return -1;
    }

    g_current->sigstate.sigframe_rsp  = frame_base;
    g_current->sigstate.siginfo_frame = 1;

    atomic_or(&g_current->sigstate.blocked, 1u << (uint32_t)(sig - 1));
    atomic_or(&g_current->sigstate.blocked, ka->sa_mask);

    redir->rip    = ka->sa_handler;
    redir->rsp    = frame_base;              // points at pretcode
    redir->rflags = 0x202;
    redir->rdi    = (uint64_t)(uint32_t)sig;
    redir->rsi    = (uint64_t)&rf->info;
    redir->rdx    = (uint64_t)&rf->uc;
    return 0;
}

// Non-SA_SIGINFO delivery: the proven simple sigframe_t.  Same source/redirect
// contract as build_rt_frame.  rsi/rdx are echoed unchanged into *redir -- the
// 1-arg handler only takes the signum in rdi, and the interrupted rsi/rdx are
// saved in the frame and restored on sigreturn, so the redirect must not
// disturb them (applying redir->rsi/rdx to the source frame is then a no-op).
static int build_simple_frame(int sig, k_sigaction_t* ka,
                              const sig_uctx_t* src, sig_redirect_t* redir) {
    g_current->sigstate.siginfo_frame = 0;
    uint64_t user_rsp = src->rsp;
    // Alt stack when SA_ONSTACK; saved frame->rsp below stays user_rsp.
    uint64_t stack_base = sig_stack_base(ka, user_rsp);

    // Skip the 128-byte red zone, then place the sigframe below it,
    // 16-byte aligned as required for stack-passed arguments.
    uint64_t frame_base = (stack_base - 128 - sizeof(sigframe_t))
                            & ~(uint64_t)0xF;

    // Validate: the whole [frame_base-8, user_rsp) window must live in the user
    // half.  If user_rsp is garbage (e.g. a buggy handler clobbered it before
    // raising another signal), writing the sigframe would clobber kernel
    // memory.  force_kill queues SIGKILL + SIG_DFL and we return -1; the caller
    // takes the terminate path.
    if (!_access_ok(frame_base - 8, sizeof(sigframe_t) + 8)) {
        signal_force_kill(sig, "setup_frame RANGE", user_rsp, frame_base); return -1;
    }

    /* Handler / restorer validation.  Both must be canonical user pointers
     * (< USER_ADDR_CEIL = 2^47), and both non-zero: a zero handler means
     * SIG_DFL (guarded earlier) and would iretq to RIP=0; a zero restorer
     * means the handler's `ret` pops 0 off the user stack → user #PF at RIP=0
     * (observed: Ctrl+C / SIGINT against dwl, CR2=0, RIP=0).  A handler
     * registered without a working restorer is a userland bug, but the kernel
     * must not crash over it — force SIGKILL and take the terminate path. */
    if (ka->sa_handler == 0 || ka->sa_handler >= USER_ADDR_CEIL ||
        ka->sa_restorer == 0 || ka->sa_restorer >= USER_ADDR_CEIL) {
        signal_force_kill(sig, "setup_frame HANDLER", user_rsp, frame_base); return -1;
    }

    // Build the frame in a kernel-local buffer and publish it with the
    // fault-safe copy_to_user, exactly as build_rt_frame does: a VALID-but-
    // unmapped or racing user stack page returns -EFAULT here instead of an
    // unrecoverable kernel #PF, so userland can never panic the kernel with a
    // bad signal-frame address.  (This replaces the old mm_vma_find pre-check,
    // which had a TOCTOU race: a sibling thread could munmap the stack between
    // the check and the raw write.)
    sigframe_t sf;
    __builtin_memset(&sf, 0, sizeof(sf));

    sf.rip    = src->rip;
    sf.rsp    = user_rsp;
    sf.rflags = src->rflags;
    sf.rbp    = src->rbp;
    sf.rbx    = src->rbx;
    sf.r12    = src->r12;
    sf.r13    = src->r13;
    sf.r14    = src->r14;
    sf.r15    = src->r15;
    // Caller-saved arg registers — the handler will clobber them; saved here so
    // sigreturn can restore the interrupted code's exact register state.
    sf.rdi    = src->rdi;
    sf.rsi    = src->rsi;
    sf.rdx    = src->rdx;
    sf.r10    = src->r10;
    sf.r8     = src->r8;
    sf.r9     = src->r9;
    sf.rax    = src->rax;
    sf.blocked = g_current->sigstate.blocked;
    sf._pad   = 0;

    // Save the interrupted FPU/SSE state.  g_current is running and the kernel
    // is -mno-sse, so its FPU registers still hold the user's live state; the
    // handler about to run will clobber them.  fxrstor in sys_sigreturn puts
    // them back.  fxsave REQUIRES a 16-byte-aligned target or it #GPs -- save
    // into an explicitly-aligned scratch and copy into the frame, exactly like
    // build_rt_frame does.  fxsave'ing straight into sf.fpu (relying on the
    // struct field's aligned(16) attribute) #GP'd the kernel when a vfork'd `sh`
    // took a signal via this legacy (non-SA_SIGINFO) path -- the field was not
    // 16-aligned on the kernel stack in practice.
    // The kernel entry stubs now keep RSP 16-byte aligned (SysV ABI) before
    // calling C, so a naturally/aligned stack local is a valid fxsave target.
    _Alignas(16) uint8_t fpu_scratch[512];
    __asm__ volatile("fxsave %0" : "=m"(fpu_scratch));
    __builtin_memcpy(sf.fpu, fpu_scratch, sizeof(sf.fpu));

    extern int copy_to_user(void* dst_u, const void* src, uint64_t len);
    // The restorer address is pushed at frame_base-8 (the handler's return
    // address); publish it together in one shot by copying the frame first,
    // then the restorer slot -- both fault-safe.
    uint64_t new_rsp  = frame_base - 8;
    uint64_t restorer = ka->sa_restorer;
    if (copy_to_user((void*)frame_base, &sf, sizeof(sf)) != 0 ||
        copy_to_user((void*)new_rsp, &restorer, sizeof(restorer)) != 0) {
        signal_force_kill(sig, "setup_frame COPY", user_rsp, frame_base); return -1;
    }

    g_current->sigstate.sigframe_rsp = frame_base;

    // Block the signal itself + sa_mask during handler execution.  Atomic so a
    // cross-CPU signal_send (SIGKILL unblock) can't lose these via a torn RMW.
    atomic_or(&g_current->sigstate.blocked, 1u << (uint32_t)(sig - 1));
    atomic_or(&g_current->sigstate.blocked, ka->sa_mask);

    redir->rip    = ka->sa_handler;
    redir->rsp    = new_rsp;
    redir->rflags = 0x202;  // IF=1, reserved
    redir->rdi    = (uint64_t)(uint32_t)sig;
    redir->rsi    = src->rsi;   // unchanged: 1-arg handler
    redir->rdx    = src->rdx;   // unchanged
    return 0;
}

// Dispatch: SA_SIGINFO handlers get the rt_sigframe (siginfo + ucontext);
// everything else keeps the proven simple frame.  Source-neutral.
static int signal_build_frame(int sig, k_sigaction_t* ka,
                              const sig_uctx_t* src, sig_redirect_t* redir) {
    if (ka->sa_flags & SA_SIGINFO) return build_rt_frame(sig, ka, src, redir);
    return build_simple_frame(sig, ka, src, redir);
}

// Syscall-return delivery: the source is the task's SYSCALL_KFRAME (which
// survives mid-syscall CPU migration), and the handler-entry redirect is
// applied back INTO it — the exit path's normal sysret/iret pop sequence then
// enters the handler.  Only call this when we are on the current task's own
// syscall-return path (may_setup_frame==1 in signal_deliver_pending).
static void signal_setup_frame(int sig, k_sigaction_t* ka, uint64_t saved_rax) {
    syscall_kframe_t* kf = SYSCALL_KFRAME(g_current);

    // This path delivers pending signals (kill/raise/tkill), not synchronous
    // faults, so the siginfo carries SI_USER (0). (Synchronous faults are
    // delivered immediately via signal_deliver_fault, which sets a fault code.)
    g_current->sigstate.fault_si_code = 0;

    sig_uctx_t src;
    src.rip = kf->rip; src.rsp = kf->rsp; src.rflags = kf->rflags;
    src.rbp = kf->rbp; src.rbx = kf->rbx;
    src.r12 = kf->r12; src.r13 = kf->r13; src.r14 = kf->r14; src.r15 = kf->r15;
    src.rdi = kf->rdi; src.rsi = kf->rsi; src.rdx = kf->rdx; src.r10 = kf->r10;
    src.r8  = kf->r8;  src.r9  = kf->r9;
    src.rax = saved_rax;
    // rcx/r11 are clobbered by `syscall` and not captured in the kframe.
    src.rcx = 0; src.r11 = 0;

    sig_redirect_t redir;
    if (signal_build_frame(sig, ka, &src, &redir) != 0) return;  // force-killed

    // Mutate the saved user context in place — the exit path consumes these.
    kf->rip    = redir.rip;
    kf->rsp    = redir.rsp;
    kf->rflags = redir.rflags;
    kf->rdi    = redir.rdi;
    kf->rsi    = redir.rsi;   // no-op for the simple frame (== saved rsi)
    kf->rdx    = redir.rdx;
}

// ── signal_deliver_fault ──────────────────────────────────────────────────
// Deliver a signal raised by a synchronous CPU exception (page fault, #DE, #UD,
// #GP, ...) whose faulting context is in the exception trap frame reachable via
// `f`.  Unlike the syscall path, we cannot just re-queue and return: iretq would
// re-execute the faulting instruction and fault again forever.  So either we
// redirect into a catchable user handler NOW, or we force the default action
// (terminate).
// True iff a `sig` raised by a synchronous fault would be delivered to a user
// handler (installed, catchable, unblocked, non-kthread) rather than terminating
// the task.  Exported so the #PF path can skip its PF-KILL diagnostic banner for
// a deliverable fault (a JVM null-checks on NULL constantly — it is not a kill).
int signal_fault_deliverable(struct task_t* t, int sig) {
    if (!t || sig < 1 || sig >= NSIG) return 0;
    if (((task_t*)t)->flags & TASK_FLAG_KTHREAD) return 0;
    const sigstate_t* ss = &((task_t*)t)->sigstate;
    uint64_t h = ((task_t*)t)->sighand->handlers[sig].sa_handler;
    if (h == (uint64_t)SIG_DFL || h == (uint64_t)SIG_IGN) return 0;
    if (ss->blocked & (1u << (uint32_t)(sig - 1))) return 0;
    return 1;
}

// POSIX si_code for a signal raised by a synchronous CPU exception. These are
// the asm-generic values every JVM/libc expects (SEGV_MAPERR=1, FPE_INTDIV=1,
// ...). A positive/kernel code (never SI_USER=0) is what tells the JVM the fault
// is a real hardware fault it may recover (implicit null check / safepoint poll)
// rather than an external kill.
static int fault_code_for(int sig) {
    switch (sig) {
        case SIGSEGV: return 1;   // SEGV_MAPERR (address not mapped)
        case SIGBUS:  return 2;   // BUS_ADRERR  (nonexistent physical address)
        case SIGFPE:  return 1;   // FPE_INTDIV  (integer divide by zero)
        case SIGILL:  return 1;   // ILL_ILLOPC  (illegal opcode)
        case SIGTRAP: return 1;   // TRAP_BRKPT  (breakpoint)
        default:      return 0x80;// SI_KERNEL   (sent by the kernel, not a user)
    }
}

void signal_deliver_fault(int sig, interrupt_frame_t* f) {
    if (!g_current || sig < 1 || sig >= NSIG) return;
    sigstate_t* ss = &g_current->sigstate;
    // Tag the siginfo this fault will carry: a real fault code, so the handler
    // (e.g. the JVM's) sees a genuine hardware fault, not SI_USER.
    ss->fault_si_code = fault_code_for(sig);
    k_sigaction_t* ka = &g_current->sighand->handlers[sig];
    uint32_t bit = 1u << (uint32_t)(sig - 1);

    // Not deliverable (SIG_DFL / SIG_IGN / blocked / kthread): a synchronous
    // fault cannot be ignored or deferred.  Force the default action and
    // terminate — this mirrors Linux's force_sig() semantics (a blocked/ignored
    // synchronous fault is reset to SIG_DFL and delivered).
    if (!signal_fault_deliverable((struct task_t*)g_current, sig)) {
        atomic_and(&ss->blocked, ~bit);
        ka->sa_handler = (uint64_t)SIG_DFL;
        signal_send(g_current, sig);
        signal_deliver_pending(0, 0);   // takes the SIG_DFL terminate path
        return;
    }

    // Catchable user handler installed and unblocked: build the signal frame
    // from the exception trap frame and redirect it so the handler runs on the
    // isr_common_entry POP_GPRS + iretq return.
    trap_frame_t* tf = TRAP_FROM_IFRAME(f);
    sig_uctx_t src;
    src.rax = tf->rax; src.rbx = tf->rbx; src.rcx = tf->rcx; src.rdx = tf->rdx;
    src.rsi = tf->rsi; src.rdi = tf->rdi; src.rbp = tf->rbp;
    src.r8  = tf->r8;  src.r9  = tf->r9;  src.r10 = tf->r10; src.r11 = tf->r11;
    src.r12 = tf->r12; src.r13 = tf->r13; src.r14 = tf->r14; src.r15 = tf->r15;
    src.rip = f->ip;   src.rsp = f->sp;   src.rflags = f->flags;

    sig_redirect_t redir;
    if (signal_build_frame(sig, ka, &src, &redir) != 0) {
        // Unusable user stack -> force-killed inside the builder; terminate now.
        signal_deliver_pending(0, 0);
        return;
    }

    // Load the handler-entry context into the trap frame.  POP_GPRS restores the
    // (now redirected) rdi/rsi/rdx; iretq returns to the handler at redir.rip
    // with the signal frame on the user stack at redir.rsp.
    f->ip    = redir.rip;
    f->sp    = redir.rsp;
    f->flags = redir.rflags;
    tf->rdi  = redir.rdi;
    tf->rsi  = redir.rsi;
    tf->rdx  = redir.rdx;
}

// ── signal_deliver_pending ────────────────────────────────────────────────
void signal_deliver_pending(int may_setup_frame, uint64_t saved_rax) {
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

    k_sigaction_t* ka = &g_current->sighand->handlers[sig < NSIG ? sig : 0];
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
        signal_setup_frame(sig, ka, saved_rax);
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
        // We ARE init (or init is absent): drain init->children ATOMICALLY --
        // it is concurrently CAS-prepended by other exiting tasks reparenting
        // their orphans, so a plain store would tear the Treiber stack.
        task_children_clear(g_current);
    }

    // Drop the fd table now so peers see EOF immediately (matching sys_exit).
    // Go through task_drop_files (the shared mechanism) so the unpublish-BEFORE-
    // release order is identical to sys_exit and cannot drift: this path used to
    // release first and NULL after (a plain store), which reopened the
    // RCU-deferred-free vs /proc/<pid>/fd-reader UAF on every fatal-signal exit.
    if (g_current->files_shared) {
        extern void kprintf(const char*, ...);
        {
            extern void task_notify_cleartid(void);
            task_notify_cleartid();
        }
        kprintf("[signal] releasing files of pid=%u comm=\"%s\" refs=%u\n",
                (unsigned)g_current->pid, g_current->comm,
                (unsigned)g_current->files_shared->refs);
        task_drop_files(g_current);
    } else {
        extern void kprintf(const char*, ...);
        kprintf("[signal] pid=%u comm=\"%s\" has NO files_shared (leak?)\n",
                (unsigned)g_current->pid, g_current->comm);
    }

    // Set exit code (-signo) + disposition via the one shared mechanism (the
    // same task_set_exit_state sys_exit uses, so the two paths cannot drift).
    // Previously this UNCONDITIONALLY zombified -- correct for a process leader
    // (the parent reaps via waitpid and is woken by SIGCHLD), but a wrong leak
    // for a TASK_FLAG_THREAD: a thread is futex-joined, never wait()ed, so it
    // lingered as an unreapable zombie (task-struct + pid leak per thread) and
    // spuriously SIGCHLD'd the process parent.  The shifted thread case now
    // self-reaps as TASK_DEAD (idle reaper frees it); the leader case is
    // unchanged (zombie + SIGCHLD under rcu_read_lock).
    task_set_exit_state(g_current, -(int32_t)sig);

    sched_yield();
    for (;;) __asm__ volatile("hlt");
}
