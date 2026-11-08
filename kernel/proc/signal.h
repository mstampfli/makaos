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
#define SA_SIGINFO   0x00000004  // 3-arg handler: (int, siginfo_t*, ucontext_t*)
#define SA_ONSTACK   0x08000000  // run the handler on the alternate signal stack
#define SA_RESTORER  0x04000000  // sa_restorer field is valid

// ── sigaltstack ss_flags ──────────────────────────────────────────────────
#define SS_ONSTACK   1   // thread is currently executing on the alt stack
#define SS_DISABLE   2   // alt stack is disabled

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
typedef struct __attribute__((aligned(16))) {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    // Caller-saved registers the x86-64 syscall ABI nonetheless preserves
    // (only rcx/r11/rax are clobbered by `syscall`).  Userland holds live
    // values in these across an inline syscall; a signal handler clobbers
    // them, so they must be saved/restored too or the interrupted code
    // resumes with garbage (observed: integer text-metric intermediates).
    uint64_t rdi, rsi, rdx, r10, r8, r9;
    // The syscall's return value.  `syscall` itself clobbers rax, but the
    // interrupted userland instruction is about to READ it (the return
    // value), so a handler clobbering rax corrupts the result unless we
    // restore it on sigreturn.
    uint64_t rax;
    uint32_t blocked;   // signal mask at time of delivery
    uint32_t _pad;
    // Interrupted FPU/SSE state (x87 + XMM), saved by FXSAVE on delivery and
    // restored by FXRSTOR on sigreturn.  The kernel is built -mno-sse, so it
    // never touches these registers; userland relies on them surviving a
    // syscall.  A signal handler clobbers them, so WITHOUT this save a signal
    // landing mid-float-computation corrupts the interrupted code's result
    // (observed: swaybar's pango text-height measurement returning garbage,
    // blowing up the bar layout so the taskbar never maps).  FXSAVE/FXRSTOR
    // require a 16-byte-aligned target: the struct is aligned(16) and the
    // user frame is placed at (rsp & ~0xF), so `fpu` (offset 80) is aligned.
    uint8_t  fpu[512] __attribute__((aligned(16)));
} sigframe_t;

// ── rt_sigframe (SA_SIGINFO delivery) ─────────────────────────────────────
// Laid out on the user stack for a SA_SIGINFO handler.  MUST match userland
// <ucontext.h> + <signal.h> byte-for-byte -- the handler reads
// uc.uc_mcontext.gregs[REG_*] and info.si_* through those headers.  The kernel
// populates it from the interrupted context and, on sys_sigreturn, restores
// FROM the (possibly handler-modified) ucontext, so a JVM's edit to
// gregs[REG_RIP] resumes execution there.  NO aligned(16) on __fpregs_mem: the
// offset must match userland exactly; 16-byte fxsave alignment is guaranteed by
// placing the frame at frame_base ≡ 8 (mod 16) in signal_setup_frame.

// gregs index order (glibc x86-64) -- MUST match userland ucontext.h REG_*.
enum {
    KREG_R8=0, KREG_R9, KREG_R10, KREG_R11, KREG_R12, KREG_R13, KREG_R14, KREG_R15,
    KREG_RDI, KREG_RSI, KREG_RBP, KREG_RBX, KREG_RDX, KREG_RAX, KREG_RCX, KREG_RSP,
    KREG_RIP, KREG_EFL, KREG_CSGSFS, KREG_ERR, KREG_TRAPNO, KREG_OLDMASK, KREG_CR2
};

typedef struct {
    int   si_signo, si_errno, si_code;
    int   si_pid, si_uid, si_status;
    void* si_addr;
    int   si_value;
} k_siginfo_t;

typedef struct {
    long long          gregs[23];
    void*              fpregs;
    unsigned long long __reserved1[8];
} k_mcontext_t;

typedef struct { void* ss_sp; int ss_flags; unsigned long ss_size; } k_stack_t;

typedef struct k_ucontext {
    unsigned long        uc_flags;
    struct k_ucontext*   uc_link;
    k_stack_t            uc_stack;
    k_mcontext_t         uc_mcontext;
    unsigned long        uc_sigmask;
    uint8_t              __fpregs_mem[512];
} k_ucontext_t;

// On the user stack (low->high): [pretcode][uc][info].  rsp -> pretcode
// (handler return address = restorer); rdi=sig, rsi=&info, rdx=&uc.
typedef struct {
    uint64_t     pretcode;
    k_ucontext_t uc;
    k_siginfo_t  info;
} k_rt_sigframe_t;

_Static_assert(__builtin_offsetof(k_mcontext_t, gregs) == 0, "gregs at mcontext+0");
_Static_assert(__builtin_offsetof(k_ucontext_t, uc_mcontext) == 40, "uc_mcontext@40");
_Static_assert(__builtin_offsetof(k_ucontext_t, __fpregs_mem) == 304, "fpregs_mem@304");
_Static_assert(__builtin_offsetof(k_rt_sigframe_t, uc) == 8, "uc after pretcode");
// gregs[KREG_RIP] absolute offset within k_rt_sigframe_t must be 8-mod-16-safe
// once frame_base ≡ 8 (mod 16); the fpstate lands at frame_base+312 ≡ 0 (mod 16).

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
    uint64_t          sigframe_rsp;      // address of the frame on the user stack
    uint64_t          fault_addr;        // CR2 of the last user #PF -> siginfo.si_addr
    int               fault_si_code;     // si_code for the next delivered siginfo:
                                         // a POSIX fault code (SEGV_MAPERR, FPE_INTDIV,
                                         // ...) for a synchronous CPU exception, or 0
                                         // (SI_USER) for a kill()/raise()-sourced signal.
                                         // Set by the delivery path before building the
                                         // frame. The JVM keys its implicit-exception
                                         // recovery on this: SI_USER makes it treat every
                                         // fault as an unrecoverable external kill.
    uint8_t           siginfo_frame;     // 1 = last delivery built an rt_sigframe
                                         // (SA_SIGINFO); sys_sigreturn restores from
                                         // its ucontext instead of the sigframe_t
    // Alternate signal stack (per-thread, POSIX sigaltstack).  A handler
    // installed with SA_ONSTACK runs on [altstack_sp, altstack_sp+altstack_size)
    // instead of the interrupted stack -- essential for catching SIGSEGV from a
    // stack overflow, when the normal stack is unusable (the JVM relies on this
    // for StackOverflowError).  altstack_sp == NULL means no alt stack (disabled).
    void*             altstack_sp;
    unsigned long     altstack_size;
} sigstate_t;

// ── Per-PROCESS signal dispositions (shared across a thread group) ─────────
// POSIX: sigaction() dispositions are per-PROCESS, not per-thread.  Every thread
// of a process shares ONE handler table (unlike the per-thread pending/blocked
// masks in sigstate_t).  Refcounted and shared exactly like task_mm_t /
// task_files_t: a pthread (sys_thread THREAD_SHARE_MM) bumps refs and shares;
// fork copies; exec resets; exit releases.  So a handler one thread installs is
// visible to all -- the JVM installs SIGSEGV/SIGFPE handlers in one thread that
// every mutator thread must honour.  Never NULL on a live task.
typedef struct {
    uint32_t      refs;                  // refcount (atomic add/sub)
    k_sigaction_t handlers[NSIG];        // per-signal user action (0 = SIG_DFL)
} sighand_t;

// ── Neutral interrupted-context capture ───────────────────────────────────
// A signal frame is built from the interrupted user register state, which can
// come from EITHER a syscall kframe (syscall-return delivery) or an exception
// trap frame (synchronous-fault delivery).  sig_uctx_t decouples the frame
// builder from the source; sig_redirect_t is the handler-entry context the
// builder produces, which the caller loads into whichever return frame it owns.
// One shared builder, so the two delivery paths cannot drift.
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rsp, rflags;
} sig_uctx_t;

typedef struct {
    uint64_t rip, rsp, rflags;   // handler entry point + stack + flags
    uint64_t rdi, rsi, rdx;      // SysV handler args: sig, &siginfo, &ucontext
} sig_redirect_t;

// ── API ───────────────────────────────────────────────────────────────────

struct task_t;
struct interrupt_frame_t;

void signal_send(struct task_t* t, int sig);
void signal_send_group(uint32_t tgid, int sig);
void signal_send_pgrp(uint32_t pgid, int sig);
// Look up `pid` and deliver `sig` atomically under rcu_read_lock so the task
// cannot be freed between lookup and delivery (closes a sched_find_pid UAF).
// Skips zombies.  Returns 0 on delivery, -ESRCH if no such live task.
int  signal_send_pid(uint32_t pid, int sig);
// POSIX kill() authorization: sender may signal target if root or same uid.
int  signal_may(const struct task_t* sender, const struct task_t* target);
// Permission-checked user kill() paths (-EPERM if not permitted).
int  signal_send_pid_user(struct task_t* sender, uint32_t pid, int sig);
void signal_send_pgrp_user(struct task_t* sender, uint32_t pgid, int sig);
void signal_deliver_pending(int may_setup_frame, uint64_t saved_rax);

// Deliver `sig` raised by a synchronous CPU exception (page fault, #DE, #UD,
// #GP, ...) to the faulting task.  If a catchable user handler is installed and
// the signal is unblocked, builds the signal frame on the user stack and
// redirects the trap frame (via `f`) into the handler so it runs on iretq --
// the JVM's SIGSEGV null-check / SIGFPE div-by-zero resume mechanism.  Otherwise
// (SIG_DFL / SIG_IGN / blocked) forces the default action and terminates: a
// synchronous fault cannot be ignored, since returning would re-fault forever.
void signal_deliver_fault(int sig, struct interrupt_frame_t* f);

// True iff a synchronous-fault `sig` would be delivered to a user handler
// (installed, catchable, unblocked) rather than terminating `t`.  The #PF path
// uses this to suppress its PF-KILL banner for a deliverable fault.
int signal_fault_deliverable(struct task_t* t, int sig);

// ── sighand_t lifecycle (shared per-process disposition table) ────────────
// alloc: fresh table, all SIG_DFL, refs=1.  copy: fresh table (refs=1) with the
// source's handlers duplicated (fork / non-shared thread).  release: drop a
// ref, free at 0.  Sharing between threads is a plain atomic refs++ at the call
// site (mirrors mm_shared / files_shared).
sighand_t* sighand_alloc(void);
sighand_t* sighand_copy(const sighand_t* src);
void       sighand_release(sighand_t* sh);

// Mask of signals whose POSIX SIG_DFL action is "ignore".  If one of these
// is pending with SIG_DFL, signal_deliver_pending silently drops it and
// blocking syscalls must NOT return EINTR for it (that was the SIGCHLD
// infinite EINTR loop bug in login — see feedback_sigchld_eintr.md).
#define SIG_DFL_IGNORE_MASK  ((1u << (SIGCHLD - 1)) | (1u << (SIGWINCH - 1)))

// Returns 1 if there is a pending signal that would actually interrupt a
// blocking syscall (has a user handler, or SIG_DFL with non-ignore default).
// Takes both the per-thread sigstate (pending/blocked) and the per-process
// sighand (dispositions), which now live in separate structs.
static inline int signal_has_actionable(const sigstate_t* ss, const sighand_t* sh) {
    uint32_t eff = ss->pending & ~ss->blocked;
    if (!eff) return 0;
    // A bit is actionable iff its handler is not SIG_IGN and it's not a
    // SIG_DFL-ignored signal (SIGCHLD/SIGWINCH with SIG_DFL).
    uint32_t mask = eff;
    while (mask) {
        int bit = __builtin_ctz(mask);
        mask &= mask - 1;
        int sig = bit + 1;
        uint64_t h = sh->handlers[sig].sa_handler;
        if (h == (uint64_t)SIG_IGN) continue;
        if (h == (uint64_t)SIG_DFL && (SIG_DFL_IGNORE_MASK & (1u << bit))) continue;
        return 1;
    }
    return 0;
}
