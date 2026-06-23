// Heap-aliasing reproducer for the intermittent OS memory-corruption bug
// confirmed in docs/AUTOFIX_LOG.md (iters 52-60): a single-allocated frame
// gets aliased across two VAs, so one process's write lands in another's
// heap (symptom: heap pointers scribbled into swaybar/pango structs -> the
// taskbar never maps).
//
// iter61 showed a pure fork+heap+brk stress PASSES, so this version mimics
// the desktop's distinctive pattern: long-lived heap-verify WORKERS running
// concurrently with fork+EXEC CHURNERS (swaybar's failing status_command
// respawns `sh` constantly; exec tears down the old COW address space and
// maps a fresh ELF).  If exec teardown aliases a freed frame into a worker's
// live heap, the worker reads a foreign value -> "[heapalias] CORRUPT".
//
// Run with no args = driver.  Run with any arg = silent exec-target (exits
// immediately, before any heap use) so churners can re-exec this same binary.
//
// PASS  -> "[heapalias] PASS"     REPRO -> "[heapalias] CORRUPT ..."

#include "libc.h"

#define WORDS   (64 * 1024)   // 256 KB region per worker
#define WITERS  3000          // verify passes per worker (long-lived)
#define NWORKER 2
#define NCHURN  8             // 2x CPUs -> heavy concurrent exec teardown
#define CITERS  900           // fork+exec cycles per churner

static int verify_worker(void) {
    unsigned long pid = (unsigned long)getpid();
    unsigned int  pat = 0xC0DE0000u | (unsigned int)(pid & 0xFFFFu);

    unsigned int* buf = (unsigned int*)malloc((size_t)WORDS * sizeof(unsigned int));
    if (!buf) { printf("[heapalias] pid=%lu OOM\n", pid); return 2; }
    for (int i = 0; i < WORDS; i++) buf[i] = pat;

    for (int it = 0; it < WITERS; it++) {
        for (int i = 0; i < WORDS; i++) {
            if (buf[i] != pat) {
                printf("[heapalias] CORRUPT worker pid=%lu it=%d off=%d got=%08x want=%08x\n",
                       pid, it, i, buf[i], pat);
                return 1;
            }
        }
        // small churn to keep faulting heap pages in/out
        void* g = malloc(8192);
        if (g) { *(volatile unsigned int*)g = pat; free(g); }
    }
    free(buf);
    return 0;
}

static int churn_exec(void) {
    const char* av[] = { "/bin/heapalias", "x", 0 };
    for (int i = 0; i < CITERS; i++) {
        pid_t p = fork();
        if (p == 0) {
            execve("/bin/heapalias", (const char* const*)av, (const char* const*)0);
            _exit(127);   // exec failed
        }
        if (p > 0) { int st = 0; wait(&st); }
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argv;
    if (argc > 1) {
        // exec-target mode: fault in a real heap (so exec/exit teardown has
        // actual page-table + leaf frames to free -> stresses the double-free
        // path), touch it, then exit silently.
        volatile unsigned char* p = (volatile unsigned char*)malloc(64 * 1024);
        if (p) { for (int i = 0; i < 64 * 1024; i += 4096) p[i] = (unsigned char)i; }
        return 0;
    }

    printf("[heapalias] start: %d workers + %d exec-churners (%d execs each)\n",
           NWORKER, NCHURN, CITERS);

    int n = 0;
    for (int k = 0; k < NWORKER; k++) {
        pid_t p = fork();
        if (p == 0) _exit(verify_worker());
        if (p > 0) n++;
    }
    for (int k = 0; k < NCHURN; k++) {
        pid_t p = fork();
        if (p == 0) _exit(churn_exec());
        if (p > 0) n++;
    }
    for (int k = 0; k < n; k++) { int st = 0; wait(&st); }

    printf("[heapalias] PASS (no aliasing; workers + exec-churn)\n");
    return 0;
}
