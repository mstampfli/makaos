// sigtest — verifies both signal-delivery paths the JVM (and the whole desktop)
// depend on:
//
//   1. Syscall-return delivery: an SA_SIGINFO handler for an asynchronous signal
//      (SIGUSR1 via raise()) runs, then sigreturn restores the interrupted
//      context so execution continues normally.  This exercises the refactored
//      signal_setup_frame -> build_rt_frame -> sys_sigreturn round trip.
//
//   2. Synchronous-fault delivery: an SA_SIGINFO handler for SIGSEGV catches a
//      NULL dereference (#PF), reads siginfo.si_addr, and rewrites
//      uc_mcontext.gregs[REG_RIP] to a recovery point.  On sigreturn the kernel
//      restores RIP from the (edited) ucontext, so execution resumes at
//      recovery() instead of re-faulting.  This is the JVM null-check mechanism
//      and exercises signal_deliver_fault (build the frame from the trap frame).
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

extern int gettid(void);

static void out(const char* s) { write(1, s, (size_t)__builtin_strlen(s)); }

// ── Path 1: syscall-return delivery (async SIGUSR1) ───────────────────────
static volatile int   g_usr1_ran   = 0;
static volatile int   g_usr1_signo = 0;

static void usr1_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig; (void)ctx;
    g_usr1_signo = info->si_signo;
    g_usr1_ran   = 1;
    // Return normally: the kernel must restore the pre-signal context so raise()
    // resumes and returns to main.
}

// ── Path 3: thread-directed delivery (pthread_kill of a helper thread) ────
static volatile int g_usr2_ran    = 0;
static volatile int g_usr2_tid    = 0;
static volatile int g_helper_tid  = 0;
static volatile int g_helper_stop = 0;

static void usr2_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig; (void)info; (void)ctx;
    g_usr2_tid = gettid();   // the thread the handler actually ran in
    g_usr2_ran = 1;
}

static void* helper_thread(void* arg) {
    (void)arg;
    g_helper_tid = gettid();
    // Spin making syscalls so an async signal can be delivered on syscall-return.
    while (!g_helper_stop) sched_yield();
    return 0;
}

// ── Path 4: alternate signal stack (sigaltstack + SA_ONSTACK) ─────────────
static char           g_altstack[16384] __attribute__((aligned(16)));
static volatile int   g_alt_ran = 0;
static volatile int   g_alt_on  = 0;   // handler's rsp was inside g_altstack?

static void alt_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig; (void)info; (void)ctx;
    char probe;                          // a local => approximates the handler rsp
    uintptr_t sp = (uintptr_t)&probe;
    g_alt_on = (sp >= (uintptr_t)g_altstack &&
                sp <  (uintptr_t)g_altstack + sizeof(g_altstack));
    g_alt_ran = 1;
}

// ── Path 2: synchronous-fault delivery (SIGSEGV on NULL) ──────────────────
static volatile int   g_si_addr_ok = 0;

static void recovery(void) {
    out(g_si_addr_ok ? "[sigtest] PASS: SA_SIGINFO delivered, si_addr==NULL, "
                       "gregs[REG_RIP] redirect resumed here\n"
                     : "[sigtest] FAIL: si_addr mismatch\n");
    _exit(g_si_addr_ok ? 0 : 1);
}

static void segv_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    ucontext_t* uc = (ucontext_t*)ctx;
    g_si_addr_ok = (info->si_addr == (void*)0);
    // Redirect resumption to recovery(): proves the kernel restores RIP from the
    // handler-edited ucontext (the JVM resume-past-fault mechanism).
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)&recovery;
}

int main(void) {
    struct sigaction sa;

    // ── Path 1: async signal via the syscall-return path ──────────────────
    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = usr1_handler;
    sa.sa_flags     = SA_SIGINFO;
    if (sigaction(SIGUSR1, &sa, 0) != 0) { out("[sigtest] FAIL: sigaction(SIGUSR1)\n"); return 1; }

    out("[sigtest] raising SIGUSR1 (syscall-return delivery)...\n");
    raise(SIGUSR1);   // delivered on this syscall's return path; handler runs, returns
    if (!g_usr1_ran || g_usr1_signo != SIGUSR1) {
        out("[sigtest] FAIL: SIGUSR1 handler did not run / bad si_signo\n");
        return 1;
    }
    // Reaching here proves sigreturn restored the interrupted context intact.
    out("[sigtest] OK: syscall-path SA_SIGINFO delivered + resumed\n");

    // ── Path 3: thread-directed delivery via pthread_kill ─────────────────
    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = usr2_handler;
    sa.sa_flags     = SA_SIGINFO;
    if (sigaction(SIGUSR2, &sa, 0) != 0) { out("[sigtest] FAIL: sigaction(SIGUSR2)\n"); return 1; }

    pthread_t helper;
    if (pthread_create(&helper, 0, helper_thread, 0) != 0) {
        out("[sigtest] FAIL: pthread_create\n"); return 1;
    }
    // Wait until the helper is running (has recorded its tid).
    for (int i = 0; i < 2000000 && g_helper_tid == 0; i++) sched_yield();
    if (g_helper_tid == 0) { out("[sigtest] FAIL: helper did not start\n"); return 1; }

    out("[sigtest] pthread_kill(helper, SIGUSR2)...\n");
    int pk = pthread_kill(helper, SIGUSR2);
    if (pk != 0) { out("[sigtest] FAIL: pthread_kill returned nonzero\n"); return 1; }

    // Wait for the handler to run (it runs in the helper thread on its next yield).
    for (int i = 0; i < 5000000 && !g_usr2_ran; i++) sched_yield();
    g_helper_stop = 1;
    pthread_join(helper, 0);
    if (!g_usr2_ran)            { out("[sigtest] FAIL: SIGUSR2 not delivered to helper\n"); return 1; }
    if (g_usr2_tid != g_helper_tid) { out("[sigtest] FAIL: SIGUSR2 ran in the wrong thread\n"); return 1; }
    out("[sigtest] OK: pthread_kill delivered to the target thread\n");

    // ── Path 4: SA_ONSTACK handler runs on the alternate signal stack ─────
    {
        stack_t ss;
        ss.ss_sp    = g_altstack;
        ss.ss_size  = sizeof(g_altstack);
        ss.ss_flags = 0;
        if (sigaltstack(&ss, 0) != 0) { out("[sigtest] FAIL: sigaltstack\n"); return 1; }
    }
    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = alt_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGUSR1, &sa, 0) != 0) { out("[sigtest] FAIL: sigaction(alt)\n"); return 1; }

    out("[sigtest] raising SIGUSR1 with SA_ONSTACK...\n");
    raise(SIGUSR1);
    if (!g_alt_ran) { out("[sigtest] FAIL: alt handler did not run\n"); return 1; }
    if (!g_alt_on)  { out("[sigtest] FAIL: handler did NOT run on the alt stack\n"); return 1; }
    out("[sigtest] OK: SA_ONSTACK handler ran on the alternate signal stack\n");

    // ── Path 2: synchronous fault via the trap-frame path ─────────────────
    __builtin_memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags     = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, 0) != 0) { out("[sigtest] FAIL: sigaction(SIGSEGV)\n"); return 1; }

    out("[sigtest] triggering NULL deref...\n");
    volatile int* p = (volatile int*)0;
    *p = 42;                       // SIGSEGV -> handler -> redirect to recovery()

    out("[sigtest] FAIL: fell through after fault\n");  // never reached
    return 1;
}
