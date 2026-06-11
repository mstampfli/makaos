// ── tls.c — thread-local storage runtime (x86-64 variant II) ─────────
//
// gcc compiles `__thread int errno;` in a static executable to
// local-exec accesses: `mov %fs:OFFSET, ...` with OFFSET resolved by
// the linker against the PT_TLS segment.  Our job at runtime:
//
//   block:  [ tls data (S bytes) ][ TCB ]
//                                  ^ %fs (thread pointer, TP)
//
// where S = align_up(memsz(PT_TLS), align(PT_TLS)) and every offset
// the linker emitted is negative relative to TP.  TCB slot 0 holds a
// self-pointer (%fs:0 == TP), which compiler-generated code and some
// ports expect.
//
// The main thread's block is static storage (TLS must exist before
// malloc — malloc sets errno).  pthread_create allocates per-thread
// blocks on the heap and the trampoline installs them via SYS_SET_FS
// before any user code runs.

#include <makaos/syscall.h>
#include <stddef.h>
#include <stdint.h>

extern char __tdata_start[], __tdata_end[], __tbss_end[];

#define TLS_ALIGN 64u
// Generous static cap for the main thread: today the segment is a
// handful of bytes (errno + a few internals).  The init traps loudly
// if it ever outgrows this.
#define MAIN_TLS_MAX 8192

static __attribute__((aligned(TLS_ALIGN)))
char s_main_tls[MAIN_TLS_MAX + 16];

size_t __makaos_tls_block_size(void) {
    size_t memsz = (size_t)(__tbss_end - __tdata_start);
    size_t aligned = (memsz + TLS_ALIGN - 1) & ~(size_t)(TLS_ALIGN - 1);
    return aligned + 16 + TLS_ALIGN;   // +TCB +alignment slack (block may
                                       // come from 16-aligned malloc)
}

// Lay a TLS image into `block` (must be TLS_ALIGN-aligned and at
// least __makaos_tls_block_size() bytes).  Returns the thread pointer.
void* __makaos_tls_setup_block(void* block) {
    size_t filesz  = (size_t)(__tdata_end - __tdata_start);
    size_t memsz   = (size_t)(__tbss_end - __tdata_start);
    size_t aligned = (memsz + TLS_ALIGN - 1) & ~(size_t)(TLS_ALIGN - 1);

    char* base = (char*)(((uintptr_t)block + TLS_ALIGN - 1)
                         & ~(uintptr_t)(TLS_ALIGN - 1));
    char* tp   = base + aligned;
    char* data = tp - memsz;
    for (size_t i = 0; i < filesz; i++) data[i] = __tdata_start[i];
    for (size_t i = filesz; i < memsz; i++) data[i] = 0;
    *(void**)tp = tp;          // TCB self-pointer (%fs:0)
    return tp;
}

// Called from crt0 before constructors and before main.
void __makaos_tls_init(void) {
    size_t need = __makaos_tls_block_size();
    if (need > sizeof(s_main_tls)) {
        // Can't use stdio (it needs TLS).  Raw write + exit.
        static const char msg[] = "fatal: TLS segment exceeds MAIN_TLS_MAX\n";
        syscall3(SYS_WRITE, 2, (uint64_t)msg, sizeof(msg) - 1);
        syscall1(SYS_EXIT, 127);
    }
    void* tp = __makaos_tls_setup_block(s_main_tls);
    syscall1(SYS_SET_FS, (uint64_t)tp);
}
