// ── tls.c — thread-local storage runtime (x86-64 variant II) ─────────
//
// gcc compiles `__thread int errno;` in a static executable to
// local-exec accesses: `mov %fs:@tpoff, ...` where the linker resolves
// @tpoff = symbol_offset_in_PT_TLS - tls_size, and
//   tls_size = align_up(p_memsz, p_align)   (PT_TLS program header).
//
// Variant II layout (block grows DOWN from the thread pointer):
//
//   [ template (p_memsz) | pad ][ TCB ]
//   ^ tp - tls_size              ^ tp = %fs   (TCB self-pointer at %fs:0)
//
// A variable at template offset O is at tp - tls_size + O.  So the
// template MUST start at tp - tls_size, NOT tp - p_memsz — when
// p_memsz isn't already p_align-aligned those differ, and every
// __thread access (errno, the pthread-key array) lands a few bytes off,
// scribbling adjacent memory (for per-thread blocks: the malloc heap).
// That mismatch was the heisenbug behind foot's font-name corruption.
//
// tls_size / p_align are read ONCE from this image's own PT_TLS via the
// auxv (AT_PHDR/AT_PHENT/AT_PHNUM the kernel puts on the initial stack)
// rather than assumed — see __makaos_tls_init.

#include <makaos/syscall.h>
#include <stddef.h>
#include <stdint.h>

extern char __tdata_start[], __tdata_end[], __tbss_end[];

// auxv keys (SysV)
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define PT_TLS    7

typedef struct { uint32_t p_type, p_flags;
                 uint64_t p_offset, p_vaddr, p_paddr,
                          p_filesz, p_memsz, p_align; } Elf64_Phdr;

// Resolved from PT_TLS at init, then reused for every per-thread block.
static size_t s_tls_size  = 0;   // align_up(p_memsz, p_align)
static size_t s_tls_align = 16;  // p_align
static size_t s_tls_filesz = 0;  // bytes of initialised .tdata

#define MAIN_TLS_MAX 16384
static __attribute__((aligned(64))) char s_main_tls[MAIN_TLS_MAX];

static inline size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

size_t __makaos_tls_block_size(void) {
    // template+pad (tls_size) + TCB(16) + alignment slack for the block.
    return s_tls_size + 16 + s_tls_align;
}

// Lay a TLS image into `block`; returns the thread pointer (%fs value).
void* __makaos_tls_setup_block(void* block) {
    char*  b  = (char*)align_up((uintptr_t)block, s_tls_align);
    char*  tp = b + s_tls_size;          // TCB sits at tp
    char*  data = tp - s_tls_size;       // template start == b
    for (size_t i = 0; i < s_tls_filesz; i++) data[i] = __tdata_start[i];
    for (size_t i = s_tls_filesz; i < s_tls_size; i++) data[i] = 0;
    *(void**)tp = tp;                    // TCB self-pointer (%fs:0)
    return tp;
}

static void die(const char* msg, unsigned n) {
    syscall3(SYS_WRITE, 2, (uint64_t)msg, n);
    syscall1(SYS_EXIT, 127);
}

// crt0 passes envp; auxv follows the envp NULL terminator.
void __makaos_tls_init(char** envp) {
    // Walk to the end of envp, then read the auxv pairs that follow.
    char** p = envp;
    while (*p) p++;
    uint64_t* aux = (uint64_t*)(p + 1);

    uint64_t phdr = 0, phent = 0, phnum = 0;
    for (; aux[0] != AT_NULL; aux += 2) {
        if (aux[0] == AT_PHDR)  phdr  = aux[1];
        else if (aux[0] == AT_PHENT) phent = aux[1];
        else if (aux[0] == AT_PHNUM) phnum = aux[1];
    }

    s_tls_filesz = (size_t)(__tdata_end - __tdata_start);
    size_t memsz = (size_t)(__tbss_end - __tdata_start);

    if (phdr && phent && phnum) {
        for (uint64_t i = 0; i < phnum; i++) {
            Elf64_Phdr* ph = (Elf64_Phdr*)(phdr + i * phent);
            if (ph->p_type == PT_TLS) {
                s_tls_align = ph->p_align ? (size_t)ph->p_align : 16;
                memsz       = (size_t)ph->p_memsz;
                s_tls_filesz = (size_t)ph->p_filesz;
                break;
            }
        }
    }
    s_tls_size = align_up(memsz, s_tls_align);

    if (__makaos_tls_block_size() > sizeof(s_main_tls))
        die("fatal: TLS segment exceeds MAIN_TLS_MAX\n", 41);

    void* tp = __makaos_tls_setup_block(s_main_tls);
    syscall1(SYS_SET_FS, (uint64_t)tp);
}
