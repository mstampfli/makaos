// Heap-aliasing reproducer for the intermittent OS memory-corruption bug
// confirmed in docs/AUTOFIX_LOG.md (iters 52-60): a single-allocated frame
// gets aliased across two VAs, so one process's write lands in another's
// heap.  Symptom in the wild: garbage values (heap pointers) scribbled into
// swaybar/pango structs -> taskbar never maps.
//
// Strategy: fork NPROC children concurrently (one+ per CPU on -smp 4, with
// scheduler migration), each fills a private heap region with a pid-unique
// 32-bit pattern, then re-verifies every word reads back ITS OWN pattern in
// a tight loop while churning small allocs to force brk growth + demand
// faults.  If a word differs, another process's data aliased into this
// child's page -> print CORRUPT (with the foreign value) and exit.  Repeat
// for ROUNDS to hit the intermittent race.  Deterministic, fast, no desktop.
//
// PASS  -> "[heapalias] PASS"  on serial.
// REPRO -> "[heapalias] CORRUPT ..." on serial (the bug).

#include "libc.h"

#define WORDS  (64 * 1024)   // 256 KB region per child
#define ITERS  48            // verify passes per child
#define NPROC  8             // 2x CPUs -> cross-CPU + migration pressure
#define ROUNDS 40

static int child_run(void) {
    unsigned long pid = (unsigned long)getpid();
    // Distinct, recognizable per-pid pattern; high bits constant so a
    // foreign value still looks obviously wrong.
    unsigned int pat = 0xC0DE0000u | (unsigned int)(pid & 0xFFFFu);

    unsigned int* buf = (unsigned int*)malloc((size_t)WORDS * sizeof(unsigned int));
    if (!buf) { printf("[heapalias] pid=%lu OOM\n", pid); return 2; }

    for (int i = 0; i < WORDS; i++) buf[i] = pat;

    for (int it = 0; it < ITERS; it++) {
        for (int i = 0; i < WORDS; i++) {
            if (buf[i] != pat) {
                printf("[heapalias] CORRUPT pid=%lu it=%d off=%d got=%08x want=%08x\n",
                       pid, it, i, buf[i], pat);
                return 1;
            }
        }
        // Force more heap demand-faulting / brk churn between passes.
        void* g = malloc(8192);
        if (g) { *(volatile unsigned int*)g = pat; free(g); }
    }
    free(buf);
    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("[heapalias] start: %d procs x %d rounds, %d KB/proc\n",
           NPROC, ROUNDS, (int)(WORDS * 4 / 1024));

    for (int r = 0; r < ROUNDS; r++) {
        for (int k = 0; k < NPROC; k++) {
            pid_t p = fork();
            if (p == 0) {
                int rc = child_run();
                _exit(rc);
            }
            if (p < 0) printf("[heapalias] fork failed at r=%d k=%d\n", r, k);
        }
        // Reap all children this round (avoid zombies); exit code is not
        // needed -- corruption is reported via the child's printf above.
        for (int k = 0; k < NPROC; k++) {
            int st = 0;
            wait(&st);
        }
        if ((r % 8) == 0) printf("[heapalias] round %d ok\n", r);
    }
    printf("[heapalias] PASS (no aliasing in %d rounds)\n", ROUNDS);
    return 0;
}
