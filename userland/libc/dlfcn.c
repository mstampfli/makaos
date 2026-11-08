// ── dlfcn.c — runtime dynamic loader (dlopen/dlsym/dlclose/dlerror) ─────────
//
// MakaOS has no separate ld.so: dlopen is a libc routine that maps a PIC .so
// into the calling process, base-relocates it, runs its init, and resolves
// dlsym via the .so's own dynamic symbol table (SysV hash).  Executables are
// PIEs and the kernel already applies THEIR relocations; this handles the .so
// side.  A .so's symbol imports bind against the loaded-.so scope (own defined
// symbols + previously dlopen'd objects); binding against the main exe's
// exported .dynsym is layered on top later.
//
// Segment mapping mirrors how the kernel maps the exe: each PT_LOAD is mmap'd
// file-backed MAP_PRIVATE with its own protection (text R+X, data R+W), so no
// mprotect is needed and W^X holds.  The BSS tail is zeroed / anon-mapped.

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

// One RECURSIVE lock serialises all loader state (the loaded-object list, the
// exe scope, refcounts).  Recursive so a DT_NEEDED dependency load, or a .so
// constructor/destructor that itself calls dlopen/dlclose, re-enters safely
// instead of self-deadlocking.
static pthread_mutex_t s_lock = { .kind = PTHREAD_MUTEX_RECURSIVE };
static void dl_lock(void)   { pthread_mutex_lock(&s_lock); }
static void dl_unlock(void) { pthread_mutex_unlock(&s_lock); }

// ── Minimal ELF64 (local to the loader; <elf.h> has the public copies) ─────
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { int64_t d_tag; uint64_t d_un; } Elf64_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_PHDR 6
#define PT_TLS 7
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define ET_DYN 3
#define SHN_UNDEF 0
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRSZ 10
#define DT_SONAME 14
#define ELF_ST_BIND(i) ((i) >> 4)
#define STB_WEAK 2
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAY 26
#define DT_FINI_ARRAYSZ 28
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define MAX_NEEDED 24
#define R_X86_64_64 1
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64  18
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define MAX_TLS_MODULES 32
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xffffffffu))

// ── Loaded-object handle ───────────────────────────────────────────────────
typedef struct dso {
    struct dso*      next;       // global list of loaded objects (dlsym scope)
    unsigned long    base;       // load bias: runtime addr = base + link vaddr
    unsigned long    map_start;  // reserved span, for munmap on dlclose
    unsigned long    map_len;
    const Elf64_Sym* symtab;     // DT_SYMTAB (runtime addr)
    const char*      strtab;     // DT_STRTAB
    uint64_t         strsz;      // DT_STRSZ -- bounds st_name (fail closed on corrupt .so)
    const uint32_t*  hash;       // DT_HASH (SysV): [nbucket, nchain, bucket[], chain[]]
    uint32_t         nsyms;      // dynsym entry count -- from DT_HASH nchain, else
                                 // derived (strtab - symtab)/syment for GNU_HASH-only
                                 // .so's (prebuilt glibc libs ship no DT_HASH)
    const unsigned long* fini_arr; // DT_FINI_ARRAY (run in reverse on dlclose)
    uint64_t         fini_sz;
    unsigned long long ino;      // (dev,ino) is the dedup key -- robust to path
    int              dev;        //   aliasing (symlinks, relative vs absolute, /lib/x vs x)
    int              refcnt;
    int              closing;    // guard: a dtor that dlclose()s its own handle
    int              tls_modid;  // dynamic-TLS module id (0 = no PT_TLS)
    char             path[192];  // resolved load path, for dladdr's dli_fname
    const char*      soname;     // DT_SONAME (into strtab); satisfies a NEEDED
                                 // already loaded from a non-/lib path (e.g. the
                                 // JDK's libjvm.so under /jdk/lib/server)
} dso_t;

// ── Dynamic TLS (general-dynamic model) ────────────────────────────────────
// A dlopen'd .so's __thread vars are accessed through __tls_get_addr(module,
// offset).  Each PT_TLS-bearing module gets an id; every thread lazily
// allocates + zero/template-inits that module's block on first touch, held in a
// per-thread DTV (a __thread array, so it lives in the static TLS block and is
// automatically per-thread).  The static executable stays local-exec (TPOFF) --
// this path is only for dynamically loaded modules.
typedef struct { unsigned long module, offset; } tls_index;
// is_static marks the main executable's module: its __thread vars live in the
// per-thread STATIC TLS (the tdata/tbss below %fs that crt0/tls.c laid out),
// reached from %fs -- NOT a lazily-malloc'd block like a dlopen'd .so's.
typedef struct { const char* tdata; unsigned long filesz, memsz; int is_static; } tls_module_t;
#define EXE_TLS_MODID 1                            // reserved id for the main exe
static tls_module_t s_tls_mod[MAX_TLS_MODULES];   // [0] unused; [1] the exe
static int          s_tls_nmod = 2;               // dynamic (.so) ids start at 2
static __thread void* g_dtv[MAX_TLS_MODULES];     // per-thread block per module (NULL = unallocated)

extern unsigned long __makaos_static_tls_size(void);  // libc: align_up(p_memsz,p_align)

// Read the thread pointer (%fs base == TCB; self-pointer stored at %fs:0).
static inline char* tls_tp(void) {
    void* tp; __asm__ __volatile__("mov %%fs:0, %0" : "=r"(tp)); return (char*)tp;
}

void* __tls_get_addr(tls_index* ti) {
    unsigned long m = ti->module;
    // Fail closed on a corrupt module id / offset (V12): bound m to the
    // registered count and the offset to the module's block size.  s_tls_mod[m]
    // and s_tls_nmod are written once under s_lock at registration, which
    // happens-before the module's code runs (via the dlopen/dlsym mutex), so
    // this reads them locklessly -- and keeps the hot TLS path lock-free.
    if (m == 0 || m >= (unsigned long)s_tls_nmod) return (void*)0;
    if (ti->offset >= s_tls_mod[m].memsz) return (void*)0;
    // The main exe's TLS is the static block: a var at PT_TLS offset O is at
    // tp - tls_size + O (variant II).  Matches the exe's own local-exec accesses
    // to the SAME var (e.g. a .so and the exe sharing libc's errno), per thread.
    if (s_tls_mod[m].is_static)
        return tls_tp() - s_tls_mod[m].memsz + ti->offset;
    void* blk = g_dtv[m];                          // __thread -> this thread only
    if (!blk) {
        tls_module_t mod = s_tls_mod[m];
        unsigned long sz = mod.memsz ? mod.memsz : 8;
        blk = malloc(sz);
        if (!blk) return (void*)0;
        if (mod.tdata) memcpy(blk, mod.tdata, mod.filesz);
        memset((char*)blk + mod.filesz, 0, sz - mod.filesz);
        g_dtv[m] = blk;
    }
    return (char*)blk + ti->offset;
}

static dso_t* s_loaded = NULL;      // head of loaded-object list (under s_lock)
static __thread char s_err[160];    // last error -- per-thread (POSIX dlerror is per-thread)
static __thread int  s_err_set = 0;

// Main-executable symbol scope: the exe exports its symbols (libc etc.) via
// --export-dynamic, so a .so's imports resolve against it.  Derived once from
// the auxv program headers libc saved (base via PT_PHDR, dynsym via PT_DYNAMIC).
extern unsigned long __libc_phdr, __libc_phent, __libc_phnum;
static dso_t s_exe;
static int   s_exe_ready = 0;

static void set_err(const char* a, const char* b) {
    // Compose "a: b" into s_err (no snprintf dependency in this TU).
    size_t i = 0;
    for (const char* p = a; *p && i < sizeof(s_err) - 1; p++) s_err[i++] = *p;
    if (b) { if (i < sizeof(s_err) - 2) { s_err[i++] = ':'; s_err[i++] = ' '; }
             for (const char* p = b; *p && i < sizeof(s_err) - 1; p++) s_err[i++] = *p; }
    s_err[i] = 0;
    s_err_set = 1;
}

static unsigned long page_down(unsigned long x) { return x & ~(unsigned long)0xFFF; }
static unsigned long page_up(unsigned long x)   { return (x + 0xFFF) & ~(unsigned long)0xFFF; }


// SysV ELF hash.
static unsigned long elf_hash(const char* s) {
    unsigned long h = 0, g;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h = (h << 4) + *p;
        if ((g = h & 0xf0000000UL)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// Look up `name` in one object's dynsym (SysV hash chain).  Returns the defined
// symbol (st_shndx != UNDEF) or NULL.
static const Elf64_Sym* dso_lookup(const dso_t* d, const char* name) {
    if (!d->symtab || !d->strtab) return NULL;
    // GNU_HASH-only object (no DT_HASH): linear scan the dynsym. Prebuilt glibc
    // libraries (LWJGL's liblwjgl.so, and many others) ship only .gnu.hash, which
    // this loader does not index; a linear scan over d->nsyms is correct (just
    // slower) and keeps such libraries loadable.
    if (!d->hash) {
        for (uint32_t i = 0; i < d->nsyms; i++) {
            const Elf64_Sym* s = &d->symtab[i];
            if (s->st_shndx != SHN_UNDEF && s->st_name < d->strsz
                && strcmp(name, d->strtab + s->st_name) == 0)
                return s;
        }
        return NULL;
    }
    uint32_t nbucket = d->hash[0];
    uint32_t nchain  = d->hash[1];          // == symtab entry count; bounds every index
    if (!nbucket || !nchain) return NULL;
    const uint32_t* bucket = d->hash + 2;
    const uint32_t* chain  = bucket + nbucket;
    // Bound every step by nchain -> no OOB read, and cap iterations so a cyclic
    // chain in a corrupt .so cannot spin forever (fail closed, V12).
    uint32_t i = bucket[elf_hash(name) % nbucket];
    for (uint32_t steps = 0; i != 0 && i < nchain && steps < nchain; i = chain[i], steps++) {
        const Elf64_Sym* s = &d->symtab[i];
        if (s->st_shndx != SHN_UNDEF && s->st_name < d->strsz
            && strcmp(name, d->strtab + s->st_name) == 0)
            return s;
    }
    return NULL;
}

// Populate the main-executable scope from the auxv program headers (once).
// exe_base = AT_PHDR - the PT_PHDR link vaddr; the exe's dynsym/strtab/hash come
// from PT_DYNAMIC.  The exe's .dynamic d_ptr fields are link-relative (the
// kernel does not relocate them), so add exe_base.
static void init_exe_scope(void) {
    if (s_exe_ready) return;
    s_exe_ready = 1;                       // attempt once; stays zeroed on failure
    if (!__libc_phdr || !__libc_phent || !__libc_phnum) return;
    unsigned long base = 0, dyn_v = 0; int have_base = 0;
    for (unsigned long i = 0; i < __libc_phnum; i++) {
        const Elf64_Phdr* p = (const Elf64_Phdr*)(__libc_phdr + i * __libc_phent);
        if (p->p_type == PT_PHDR)    { base = __libc_phdr - p->p_vaddr; have_base = 1; }
        if (p->p_type == PT_DYNAMIC) dyn_v = p->p_vaddr;
    }
    if (!have_base || !dyn_v) return;
    for (const Elf64_Dyn* e = (const Elf64_Dyn*)(base + dyn_v); e->d_tag != DT_NULL; e++) {
        switch (e->d_tag) {
            case DT_SYMTAB: s_exe.symtab = (const Elf64_Sym*)(base + e->d_un); break;
            case DT_STRTAB: s_exe.strtab = (const char*)(base + e->d_un); break;
            case DT_STRSZ:  s_exe.strsz  = e->d_un; break;
            case DT_HASH:   s_exe.hash   = (const uint32_t*)(base + e->d_un); break;
        }
    }
    s_exe.base = base;
    // Register the main exe as TLS module EXE_TLS_MODID so a .so's cross-module
    // __thread import (e.g. libc's errno) resolves here.  Its block is the static
    // TLS (reached from %fs), sized by libc's authoritative align_up(p_memsz).
    s_tls_mod[EXE_TLS_MODID].is_static = 1;
    s_tls_mod[EXE_TLS_MODID].memsz     = __makaos_static_tls_size();
}

// Resolve `name` to a runtime address: loaded objects first (global scope), then
// the main executable's exported symbols.  Returns 0 if unresolved.
static unsigned long resolve_sym(const char* name) {
    for (dso_t* d = s_loaded; d; d = d->next) {
        const Elf64_Sym* s = dso_lookup(d, name);
        if (s) return d->base + s->st_value;
    }
    init_exe_scope();
    if (s_exe.hash) {
        const Elf64_Sym* s = dso_lookup(&s_exe, name);
        if (s) return s_exe.base + s->st_value;
    }
    return 0;
}

// Resolve a cross-module __thread symbol (a general-dynamic TLS reloc whose
// symbol is UNDEFINED in the referencing object, e.g. a .so importing libc's
// `errno`) to its DEFINING module: the (module id, in-block offset) pair that
// __tls_get_addr consumes.  For a TLS symbol st_value IS the PT_TLS-block offset
// (not a load address), so it is used directly -- never base-relocated.  Global
// scope order matches resolve_sym: loaded .so's first, then the main exe.
// Returns 1 on success.
static int tls_resolve(const char* name, unsigned long* mod, unsigned long* off) {
    for (dso_t* e = s_loaded; e; e = e->next) {
        const Elf64_Sym* s = dso_lookup(e, name);
        if (s && e->tls_modid) { *mod = (unsigned long)e->tls_modid; *off = s->st_value; return 1; }
    }
    init_exe_scope();
    if (s_exe.hash && s_tls_mod[EXE_TLS_MODID].is_static) {
        const Elf64_Sym* s = dso_lookup(&s_exe, name);
        if (s) { *mod = EXE_TLS_MODID; *off = s->st_value; return 1; }
    }
    return 0;
}

// Map every PT_LOAD from `fd` at load bias `base`.  Handles the BSS tail (zero
// the last file page, anon-map the extra pages).  Returns 0 / -1.
static int map_segments(int fd, unsigned long base, const Elf64_Phdr* ph, int phnum) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        int prot = 0;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;

        unsigned long vaddr      = base + ph[i].p_vaddr;
        unsigned long seg_beg    = page_down(vaddr);
        unsigned long file_end   = vaddr + ph[i].p_filesz;
        unsigned long mem_end    = vaddr + ph[i].p_memsz;
        unsigned long file_pg_end = page_up(file_end);
        unsigned long mem_pg_end  = page_up(mem_end);
        uint64_t      file_off   = page_down(ph[i].p_offset);

        // File-backed part [seg_beg, file_pg_end), MAP_FIXED over the reservation.
        if (file_pg_end > seg_beg) {
            void* r = mmap((void*)seg_beg, file_pg_end - seg_beg, prot,
                           MAP_PRIVATE | MAP_FIXED, fd, (long)file_off);
            if (r == MAP_FAILED) return -1;
        }
        // Zero the BSS bytes sharing the last file page (writable segments).
        if ((prot & PROT_WRITE) && file_end < file_pg_end && ph[i].p_memsz > ph[i].p_filesz)
            memset((void*)file_end, 0, file_pg_end - file_end);
        // Extra whole BSS pages beyond the file-backed part: anon zero.
        if (mem_pg_end > file_pg_end) {
            void* r = mmap((void*)file_pg_end, mem_pg_end - file_pg_end, prot,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (r == MAP_FAILED) return -1;
        }
    }
    return 0;
}

// Apply one Rela table.  Returns 0 / -1 (missing symbol or unsupported type).
static int apply_rela(dso_t* d, const Elf64_Rela* rela, uint64_t sz) {
    if (!rela || !sz) return 0;
    for (uint64_t o = 0; o + sizeof(Elf64_Rela) <= sz; o += sizeof(Elf64_Rela)) {
        const Elf64_Rela* r = (const Elf64_Rela*)((const uint8_t*)rela + o);
        uint32_t type = ELF64_R_TYPE(r->r_info);
        // The reloc target must lie inside this object's own mapped span, or a
        // corrupt .so would scribble the exe/heap.  Fail closed (V12).
        unsigned long w = d->base + r->r_offset;
        if (w < d->map_start || w + sizeof(uint64_t) > d->map_start + d->map_len) {
            set_err("dlopen: reloc offset out of range", 0); return -1;
        }
        uint64_t* where = (uint64_t*)w;
        if (type == R_X86_64_RELATIVE) { *where = d->base + (uint64_t)r->r_addend; continue; }

        // Dynamic-TLS relocations (general-dynamic): DTPMOD64 = the module id of
        // the object whose TLS the __thread var lives in; DTPOFF64 = its offset
        // within that module's block.  Together they are the tls_index that
        // __tls_get_addr resolves per-thread.  Handled before symbol resolution
        // because DTPMOD64 usually has sym 0 (this module's own TLS).
        if (type == R_X86_64_DTPMOD64 || type == R_X86_64_DTPOFF64 || type == R_X86_64_TPOFF64) {
            if (type == R_X86_64_TPOFF64) {
                set_err("dlopen: initial-exec TLS (TPOFF64) in a .so unsupported", 0); return -1;
            }
            uint32_t tsym = ELF64_R_SYM(r->r_info);
            const Elf64_Sym* ts = (tsym && tsym < d->nsyms) ? &d->symtab[tsym] : (const Elf64_Sym*)0;
            unsigned long mod, off;
            if (ts && ts->st_shndx == SHN_UNDEF) {
                // Cross-module __thread import (e.g. libc's errno): the var lives
                // in whichever module DEFINES it, not here.  Resolve globally.
                const char* nm = (ts->st_name < d->strsz) ? d->strtab + ts->st_name : "";
                if (!tls_resolve(nm, &mod, &off)) {
                    // A weak undefined TLS import legitimately yields module 0 /
                    // offset 0 (__tls_get_addr returns NULL); a strong one is fatal.
                    if (ELF_ST_BIND(ts->st_info) == STB_WEAK) { mod = 0; off = 0; }
                    else { set_err("dlopen: undefined TLS symbol", nm); return -1; }
                }
            } else {
                // Own module: a __thread var defined here (or a STB_LOCAL access
                // with sym 0).  st_value is its offset within this .so's block.
                mod = (unsigned long)d->tls_modid;
                off = ts ? ts->st_value : 0;
            }
            if (type == R_X86_64_DTPMOD64) *where = (uint64_t)mod;
            else                           *where = off + (uint64_t)r->r_addend;
            continue;
        }

        // Bound the symbol index + name against the .so's tables (OOB guard).
        uint32_t symi = ELF64_R_SYM(r->r_info);
        if (!d->nsyms || symi >= d->nsyms) { set_err("dlopen: bad reloc symbol index", 0); return -1; }
        const Elf64_Sym* s = &d->symtab[symi];
        if (s->st_name >= d->strsz) { set_err("dlopen: bad reloc symbol name", 0); return -1; }
        const char* name = d->strtab + s->st_name;
        const Elf64_Sym* def = dso_lookup(d, name);
        unsigned long val = def ? d->base + def->st_value : resolve_sym(name);
        // A WEAK undefined symbol legitimately resolves to 0; only a STRONG
        // undefined that no object provides is fatal.
        if (!val && s->st_shndx == SHN_UNDEF && ELF_ST_BIND(s->st_info) != STB_WEAK) {
            set_err("dlopen: undefined symbol", name); return -1;
        }
        switch (type) {
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: *where = val; break;
            case R_X86_64_64:        *where = val + (uint64_t)r->r_addend; break;
            default: set_err("dlopen: unsupported reloc type", name); return -1;
        }
    }
    return 0;
}

// Run an init/fini function-pointer array (DT_INIT_ARRAY / DT_FINI_ARRAY),
// bounded to the object's own mapped span so a corrupt array pointer in a bad
// .so cannot make us fetch + call through wild memory (V12).  `reverse` runs
// destructors last-to-first.
static void run_array(const dso_t* d, const unsigned long* arr, uint64_t sz, int reverse) {
    if (!arr || !sz) return;
    unsigned long a = (unsigned long)arr;
    if (a < d->map_start || a + sz > d->map_start + d->map_len) return;   // corrupt -> skip
    uint64_t n = sz / sizeof(unsigned long);
    for (uint64_t k = 0; k < n; k++) {
        unsigned long fp = arr[reverse ? (n - 1 - k) : k];
        if (fp && fp != ~0UL) ((void (*)(void))fp)();
    }
}

// dlopen(NULL) returns this handle for the global scope (main exe + all loaded
// objects); dlsym on it searches everything via resolve_sym.
static dso_t s_global;

// Resolve a DT_NEEDED name to a path: absolute if it has a '/', else /lib/<name>.
static void dep_path(const char* name, char* out, unsigned outsz) {
    unsigned i = 0; int slash = 0;
    for (const char* p = name; *p; p++) if (*p == '/') slash = 1;
    if (!slash) for (const char* p = "/lib/"; *p && i + 1 < outsz; p++) out[i++] = *p;
    for (const char* p = name; *p && i + 1 < outsz; p++) out[i++] = *p;
    out[i] = 0;
}

// Remove `d` from the loaded list (used on a failed load).
static void unpublish(dso_t* d) {
    for (dso_t** pp = &s_loaded; *pp; pp = &(*pp)->next)
        if (*pp == d) { *pp = d->next; break; }
}

// Core loader, called with s_lock held (the public dlopen wrapper + DT_NEEDED
// recursion both come through here).
static void* dlopen_impl(const char* path, int flags) {
    (void)flags;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { set_err("dlopen: cannot open", path); return NULL; }

    // Dedup by (dev, ino): a re-dlopen of the same underlying file -- via any
    // path alias -- bumps the refcount and returns the same handle (also breaks
    // circular DT_NEEDED chains, since the object is already published below).
    struct stat st;
    if (fstat(fd, &st) != 0) { set_err("dlopen: fstat failed", path); close(fd); return NULL; }
    for (dso_t* e = s_loaded; e; e = e->next)
        if (e->ino == st.st_ino && e->dev == st.st_dev) { close(fd); e->refcnt++; return e; }

    Elf64_Ehdr eh;
    if (lseek(fd, 0, 0) != 0 || read(fd, &eh, sizeof eh) != (long)sizeof eh
        || eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E'
        || eh.e_type != ET_DYN || eh.e_phnum == 0) {
        set_err("dlopen: not a shared object", path); close(fd); return NULL;
    }
    int phnum = eh.e_phnum;
    Elf64_Phdr* ph = (Elf64_Phdr*)malloc((size_t)phnum * sizeof(Elf64_Phdr));
    if (!ph) { set_err("dlopen: out of memory", 0); close(fd); return NULL; }
    size_t phsz = (size_t)phnum * sizeof(Elf64_Phdr);
    if (lseek(fd, (long)eh.e_phoff, 0) != (long)eh.e_phoff
        || read(fd, ph, phsz) != (long)phsz) {
        set_err("dlopen: short phdr read", path); free(ph); close(fd); return NULL;
    }

    // Span of all PT_LOADs (link-time vaddrs) + PT_DYNAMIC + PT_TLS template.
    unsigned long lo = ~0UL, hi = 0, dyn_vaddr = 0;
    unsigned long tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn_vaddr = ph[i].p_vaddr;
        if (ph[i].p_type == PT_TLS) { tls_vaddr = ph[i].p_vaddr; tls_filesz = ph[i].p_filesz; tls_memsz = ph[i].p_memsz; }
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (page_down(ph[i].p_vaddr) < lo) lo = page_down(ph[i].p_vaddr);
        if (page_up(ph[i].p_vaddr + ph[i].p_memsz) > hi) hi = page_up(ph[i].p_vaddr + ph[i].p_memsz);
    }
    if (lo == ~0UL || hi <= lo || !dyn_vaddr) {
        set_err("dlopen: malformed program headers", path); free(ph); close(fd); return NULL;
    }

    // Reserve the whole span, then MAP_FIXED each segment over it.
    size_t span = hi - lo;
    void* res = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) { set_err("dlopen: address reservation failed", path); free(ph); close(fd); return NULL; }
    unsigned long base = (unsigned long)res - lo;   // base + p_vaddr lands in [res, res+span)

    if (map_segments(fd, base, ph, phnum) != 0) {
        set_err("dlopen: segment mmap failed", path);
        munmap(res, span); free(ph); close(fd); return NULL;
    }
    free(ph);
    close(fd);   // segments are mapped; their VMAs hold their own file refs

    dso_t* d = (dso_t*)malloc(sizeof(dso_t));
    if (!d) { set_err("dlopen: out of memory", 0); munmap(res, span); return NULL; }
    memset(d, 0, sizeof *d);
    d->base = base; d->map_start = (unsigned long)res; d->map_len = span; d->refcnt = 1;
    d->ino = st.st_ino; d->dev = st.st_dev;   // dedup key
    // Retain the load path so dladdr can report dli_fname (OpenJDK's os::jvm_path
    // maps a libjvm.so code address back to its .so path to derive JAVA_HOME).
    { size_t pl = 0; while (path[pl] && pl < sizeof(d->path) - 1) { d->path[pl] = path[pl]; pl++; } d->path[pl] = '\0'; }

    // Register the .so's TLS block as a dynamic-TLS module (id in DTPMOD64
    // relocs).  s_tls_mod is written under s_lock, which dlopen_impl holds.
    if (tls_memsz > 0) {
        if (s_tls_nmod >= MAX_TLS_MODULES) {
            set_err("dlopen: too many TLS modules", path);
            munmap(res, span); free(d); return NULL;
        }
        int mid = s_tls_nmod++;
        s_tls_mod[mid].tdata  = (const char*)(base + tls_vaddr);
        s_tls_mod[mid].filesz = tls_filesz;
        s_tls_mod[mid].memsz  = tls_memsz;
        d->tls_modid = mid;
    }

    // Parse the dynamic section (resident at base + dyn_vaddr).
    const Elf64_Dyn* dyn = (const Elf64_Dyn*)(base + dyn_vaddr);
    const Elf64_Rela* rela = NULL; uint64_t rela_sz = 0;
    const Elf64_Rela* jmprel = NULL; uint64_t jmprel_sz = 0;
    const unsigned long* init_arr = NULL; uint64_t init_sz = 0;
    uint64_t needed[MAX_NEEDED]; int n_needed = 0, needed_overflow = 0;
    uint64_t soname_off = 0;
    for (const Elf64_Dyn* e = dyn; e->d_tag != DT_NULL; e++) {
        switch (e->d_tag) {
            case DT_NEEDED:
                if (n_needed < MAX_NEEDED) needed[n_needed++] = e->d_un;
                else needed_overflow = 1;
                break;
            case DT_SONAME:       soname_off = e->d_un; break;
            case DT_SYMTAB:       d->symtab = (const Elf64_Sym*)(base + e->d_un); break;
            case DT_STRTAB:       d->strtab = (const char*)(base + e->d_un); break;
            case DT_STRSZ:        d->strsz  = e->d_un; break;
            case DT_HASH:         d->hash   = (const uint32_t*)(base + e->d_un); break;
            case DT_RELA:         rela      = (const Elf64_Rela*)(base + e->d_un); break;
            case DT_RELASZ:       rela_sz   = e->d_un; break;
            case DT_JMPREL:       jmprel    = (const Elf64_Rela*)(base + e->d_un); break;
            case DT_PLTRELSZ:     jmprel_sz = e->d_un; break;
            case DT_INIT_ARRAY:   init_arr  = (const unsigned long*)(base + e->d_un); break;
            case DT_INIT_ARRAYSZ: init_sz   = e->d_un; break;
            case DT_FINI_ARRAY:   d->fini_arr = (const unsigned long*)(base + e->d_un); break;
            case DT_FINI_ARRAYSZ: d->fini_sz  = e->d_un; break;
        }
    }
    if (needed_overflow) {   // never silently drop deps -> unresolved symbols; fail closed
        set_err("dlopen: too many DT_NEEDED", path);
        munmap(res, span); free(d); return NULL;
    }
    // Resolve our own SONAME so later NEEDED lookups can match us by name even
    // when we were loaded from a non-standard path (the JDK's libjvm.so lives at
    // /jdk/lib/server, but its dependents just NEED "libjvm.so").
    d->soname = (soname_off && d->strtab && soname_off < d->strsz) ? (d->strtab + soname_off) : NULL;

    // Symbol count. From DT_HASH's nchain when present; otherwise (GNU_HASH-only,
    // e.g. prebuilt glibc .so's) derive it from the dynsym/dynstr layout -- the
    // linker lays .dynstr immediately after the contiguous .dynsym, so
    // (strtab - symtab)/syment is the entry count.
    if (d->hash)
        d->nsyms = d->hash[1];
    else if (d->symtab && d->strtab && (unsigned long)d->strtab > (unsigned long)d->symtab)
        d->nsyms = (uint32_t)(((unsigned long)d->strtab - (unsigned long)d->symtab)
                              / sizeof(Elf64_Sym));

    // Publish BEFORE loading dependencies + relocating so the object's own
    // symbols are visible to its deps' relocations, and a circular DT_NEEDED
    // chain dedups against the already-published object instead of recursing.
    d->next = s_loaded; s_loaded = d;

    // Load DT_NEEDED dependencies (recursively); each publishes itself, so this
    // object's relocations below can bind against them.
    for (int i = 0; i < n_needed; i++) {
        if (!d->strtab || needed[i] >= d->strsz) continue;
        const char* nm = d->strtab + needed[i];
        // glibc core sonames (libc.so.6, libpthread.so.0, libdl.so.2, libm.so.6,
        // librt.so.1, ld-linux-x86-64.so.2) are provided by the process's own
        // statically-linked libc -- there is no such .so file on MakaOS. A
        // prebuilt glibc library (LWJGL's liblwjgl.so, etc.) NEEDs them; treat
        // them as already satisfied so those libraries load. Match "libX.so"
        // prefix so the version suffix does not matter.
        {
            static const char* const glibc_core[] = {
                "libc.so", "libpthread.so", "libdl.so", "libm.so",
                "librt.so", "ld-linux", "libresolv.so", "libutil.so", 0 };
            int is_glibc = 0;
            for (int g = 0; glibc_core[g]; g++) {
                const char* a = glibc_core[g]; const char* b = nm;
                while (*a && *a == *b) { a++; b++; }
                if (*a == 0) { is_glibc = 1; break; }
            }
            if (is_glibc) continue;
        }
        // Already satisfied by an object loaded earlier (matched by SONAME)?
        // ld.so semantics: a NEEDED that is already present is not reloaded --
        // and critically, is NOT sought again by path (so a dep loaded from
        // /jdk/lib/server by absolute path still satisfies a bare-soname NEEDED).
        int already = 0;
        for (dso_t* q = s_loaded; q; q = q->next) {
            if (q == d || !q->soname) continue;
            const char* a = q->soname; const char* b = nm;
            while (*a && *a == *b) { a++; b++; }
            if (*a == *b) { already = 1; break; }
        }
        if (already) continue;
        // Resolve the NEEDED path. First try the loading object's OWN directory
        // ($ORIGIN / DT_RPATH semantics): the JDK ships libnio.so NEEDing
        // libnet.so with both in /jdk/lib, but a bare soname otherwise resolves
        // only to /lib. Fall back to dep_path (/lib/<soname>) if not found there.
        char dp[192];
        int resolved = 0;
        {
            int slash = -1;
            for (int k = 0; d->path[k]; k++) if (d->path[k] == '/') slash = k;
            if (slash >= 0) {
                int j = 0;
                for (int k = 0; k <= slash && j < (int)sizeof(dp) - 1; k++) dp[j++] = d->path[k];
                for (int k = 0; nm[k] && j < (int)sizeof(dp) - 1; k++) dp[j++] = nm[k];
                dp[j] = '\0';
                struct stat st2;
                if (stat(dp, &st2) == 0) resolved = 1;
            }
        }
        if (!resolved) dep_path(nm, dp, sizeof dp);
        if (!dlopen_impl(dp, flags)) {   // already under s_lock
            set_err("dlopen: dependency failed", dp);
            unpublish(d); munmap(res, span); free(d); return NULL;
        }
    }

    if (apply_rela(d, rela, rela_sz) != 0 || apply_rela(d, jmprel, jmprel_sz) != 0) {
        unpublish(d); munmap(res, span); free(d); return NULL;
    }

    run_array(d, init_arr, init_sz, 0);   // constructors (DT_INIT_ARRAY), bounded
    return d;
}

void* dlopen(const char* path, int flags) {
    s_err_set = 0;
    if (!path) return &s_global;   // global-scope handle (RTLD_DEFAULT-like)
    // A bare soname (no '/') searches the library dir, like standard dlopen and
    // our own DT_NEEDED resolution -- so dlopen("libSDL3.so") finds /lib/libSDL3.so
    // (sdl2-compat and other libraries dlopen by soname, unmodified).  A path with
    // a '/' (absolute or relative) is used as-is.  Same rule as dep_path (DRY).
    char resolved[128];
    dep_path(path, resolved, sizeof resolved);
    dl_lock();
    void* r = dlopen_impl(resolved, flags);
    dl_unlock();
    return r;
}

void* dlsym(void* handle, const char* name) {
    s_err_set = 0;
    if (!name) { set_err("dlsym: bad argument", 0); return NULL; }
    dl_lock();
    void* r = NULL;
    // RTLD_DEFAULT ((void*)0) and RTLD_NEXT ((void*)-1) request the global scope
    // (main exe + every loaded object) rather than one object's table. resolve_sym
    // already walks s_loaded then s_exe, which is exactly the default scope; a
    // precise RTLD_NEXT (skip up to the caller's object) is approximated by it,
    // since no in-tree consumer relies on the skip. This is essential for OpenJDK,
    // which resolves sched_getcpu/sigaction/JVM_* via dlsym(RTLD_DEFAULT, ...) --
    // a NULL return there sends HotSpot down its vsyscall-getcpu fallback, which
    // faults (MakaOS maps no vsyscall page).
    if (handle == RTLD_DEFAULT || handle == RTLD_NEXT || handle == &s_global) {
        unsigned long v = resolve_sym(name);
        if (v) r = (void*)v;
    } else {
        const Elf64_Sym* s = dso_lookup((dso_t*)handle, name);
        if (s) r = (void*)(((dso_t*)handle)->base + s->st_value);
    }
    dl_unlock();
    if (!r) set_err("dlsym: symbol not found", name);
    return r;
}

int dlclose(void* handle) {
    if (!handle || handle == &s_global) return 0;
    dl_lock();
    dso_t* d = (dso_t*)handle;
    if (--d->refcnt <= 0) {
        if (d->closing) { dl_unlock(); return 0; }  // re-entrant self-close from a dtor
        d->closing = 1;
        run_array(d, d->fini_arr, d->fini_sz, 1);   // destructors (DT_FINI_ARRAY), reverse, bounded
        // Note: DT_NEEDED deps are left loaded (no per-object dep list yet); a
        // small leak, not a correctness bug.
        unpublish(d);
        munmap((void*)d->map_start, d->map_len);
        free(d);
    }
    dl_unlock();
    return 0;
}

char* dlerror(void) {
    if (!s_err_set) return NULL;
    s_err_set = 0;
    return s_err;
}

// dladdr: map a runtime address to the loaded object containing it, reporting
// the object's path (dli_fname), load base (dli_fbase), and the nearest
// preceding exported symbol (dli_sname/dli_saddr). Walks the dlopen object list
// only -- addresses in the main executable are not resolved (no consumer needs
// that yet; OpenJDK's os::jvm_path queries a libjvm.so address, which is a
// dlopen'd object). Returns nonzero on success, 0 if the address is unknown
// (glibc convention -- inverted from the usual 0-is-success).
int dladdr(const void* addr, Dl_info* info) {
    if (!info) return 0;
    info->dli_fname = 0; info->dli_fbase = 0; info->dli_sname = 0; info->dli_saddr = 0;
    unsigned long a = (unsigned long)addr;
    dl_lock();
    for (dso_t* d = s_loaded; d; d = d->next) {
        if (a < d->map_start || a >= d->map_start + d->map_len) continue;
        info->dli_fname = d->path;
        info->dli_fbase = (void*)d->base;
        // Nearest defined symbol at or below `a` (best-effort; needs DT_HASH,
        // whose nchain bounds the symbol table -- present since we build .so's
        // with --hash-style=both).
        if (d->symtab && d->hash && d->strtab) {
            uint32_t nsym = d->nsyms;
            const Elf64_Sym* best = 0; unsigned long best_val = 0;
            for (uint32_t i = 0; i < nsym; i++) {
                const Elf64_Sym* s = &d->symtab[i];
                if (s->st_shndx == 0 || s->st_name >= d->strsz) continue;  // undefined / corrupt
                unsigned long v = d->base + s->st_value;
                if (v <= a && v >= best_val) { best = s; best_val = v; }
            }
            if (best) { info->dli_sname = d->strtab + best->st_name; info->dli_saddr = (void*)best_val; }
        }
        dl_unlock();
        return 1;
    }
    dl_unlock();
    return 0;
}

// dlvsym: versioned symbol lookup. MakaOS has no symbol versioning, so we
// ignore the requested version and resolve the plain name.
void* dlvsym(void* handle, const char* name, const char* version) {
    (void)version;
    return dlsym(handle, name);
}

// dlinfo: MakaOS's loader does not yet expose a link map, so information
// requests (e.g. RTLD_DI_LINKMAP) fail. Callers use this only for diagnostics.
int dlinfo(void* handle, int request, void* info) {
    (void)handle; (void)request; (void)info;
    return -1;
}

// dl_iterate_phdr: enumerate loaded objects. MakaOS's loader does not yet
// expose its link map for iteration, so we visit nothing and return 0. This
// degrades gracefully: the only in-tree consumer (OpenJDK os_linux.cpp) uses
// it to map a code address to its shared-object name for diagnostics, which
// simply comes back empty. TODO: walk the dlopen object list + the main-exe
// PT_LOAD headers once the loader publishes them.
//
// struct dl_phdr_info (defined in <link.h>) is used only as a pointer here, so
// a forward declaration keeps this file off the <elf.h> include chain and away
// from the loader's own local ELF typedefs above.
struct dl_phdr_info;
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info* info, unsigned long size,
                                    void* data),
                    void* data) {
    (void)callback;
    (void)data;
    return 0;
}
