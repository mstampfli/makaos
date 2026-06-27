#include "elf.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "kheap.h"
#include "vfs.h"
#include "ext2.h"
#include "process.h"
#include "tss.h"
#include "common.h"
#include "tty.h"
#include "sched.h"
#include "perm.h"
#include "cred.h"
#include "ksec.h"
#include "kprintf.h"
#include "tsc.h"

// Returns 1 if the program-header table [e_phoff, e_phoff + e_phnum*sizeof
// (Elf64_Phdr)) lies fully within a file of `size` bytes with NO 64-bit
// overflow; 0 otherwise.  The old inline check `e_phoff + phnum*56 > size`
// added unchecked 64-bit values, so an attacker-controlled e_phoff near
// UINT64_MAX (e.g. -56) wrapped below `size` and passed -> the phdr reads
// landed BEFORE the header buffer (OOB heap read).  Pure + bounds-only so it
// can be unit-tested in isolation (elf_phtab_bounds_selftest).  e_phnum is at
// most 65535, so e_phnum*56 cannot overflow.
int elf_phtab_in_bounds(uint64_t e_phoff, uint16_t e_phnum, uint64_t size) {
    if (e_phnum == 0) return 0;
    uint64_t tab = (uint64_t)e_phnum * sizeof(Elf64_Phdr);
    if (e_phoff > size) return 0;          // offset alone past EOF (catches the wrap)
    if (tab > size - e_phoff) return 0;    // overflow-safe remainder check
    return 1;
}

// Returns 1 if a PT_LOAD segment [vaddr+load_bias, +memsz) lies fully within
// [0, user_top) with no 64-bit overflow; 0 otherwise.  The mapping code below
// computes `(p_vaddr + load_bias + p_memsz + PAGE_MASK)`, which can overflow
// (seg_end < seg_start -> a bogus/empty VMA), and never range-checked p_vaddr
// (a kernel-half / non-canonical VA -> an un-faultable mapping).  Pure +
// bounds-only so it is unit-testable.
int elf_seg_range_ok(uint64_t vaddr, uint64_t load_bias,
                     uint64_t memsz, uint64_t user_top) {
    uint64_t vbase = vaddr + load_bias;
    if (vbase < vaddr) return 0;             // vaddr + load_bias overflow
    if (vbase >= user_top) return 0;         // base in kernel / non-canonical half
    if (memsz > user_top - vbase) return 0;  // base + memsz past user_top (overflow-safe)
    return 1;
}

#ifdef MAKAOS_BOOT_SELFTESTS
// Deterministic test of the phdr-table bounds.  The 0xFFFF...C8 (-56) row is
// the exact overflow the old `e_phoff + phnum*56 > size` check got wrong: it
// wrapped to 0, passed, and read 56 bytes before the buffer.
void elf_phtab_bounds_selftest(void) {
    kprintf("[elf_test] program-header table bounds\n");
    struct { uint64_t off; uint16_t n; uint64_t sz; int want; } c[] = {
        { 64,                     1,  4096, 1 },  // normal
        { 0xFFFFFFFFFFFFFFC8ULL,  1,  4096, 0 },  // -56: old check wrapped to 0 (the bug)
        { 4096,                   1,  4096, 0 },  // off == size, no room
        { 4040,                   1,  4096, 1 },  // last 56 bytes fit exactly
        { 4041,                   1,  4096, 0 },  // one past
        { 0,                      0,  4096, 0 },  // phnum 0
        { 0,                      73, 4096, 1 },  // 73*56 = 4088 <= 4096
        { 0,                      74, 4096, 0 },  // 74*56 = 4144 > 4096
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        int got = elf_phtab_in_bounds(c[i].off, c[i].n, c[i].sz);
        if (got != c[i].want) {
            kprintf("[elf_test] FAIL off=0x%lx n=%u sz=%lu got=%d want=%d\n",
                    (unsigned long)c[i].off, (unsigned)c[i].n,
                    (unsigned long)c[i].sz, got, c[i].want);
            fails++;
        }
    }

    // PT_LOAD segment-range bounds.  The overflow rows are the exact cases the
    // old un-checked `(p_vaddr + load_bias + p_memsz + PAGE_MASK)` would wrap.
    const uint64_t TOP = VMM_USER_STACK_TOP;
    struct { uint64_t va; uint64_t bias; uint64_t msz; int want; } s[] = {
        { 0x400000,            0,                TOP,        0 },  // memsz spans whole user half -> past top
        { 0x400000,            0,                0x1000,     1 },  // normal fixed-load segment
        { 0x1000,              0x555555554000,   0x2000,     1 },  // PIE load_bias, fits
        { 0xFFFF800000000000,  0,                0x1000,     0 },  // base in the kernel half
        { 0xFFFFFFFFFFFFF000,  0x2000,           0x1000,     0 },  // vaddr + bias wraps past 2^64
        { TOP - 0x1000,        0,                0x1000,     1 },  // last user page, exact fit
        { TOP,                 0,                1,          0 },  // base == top (one past the ceiling)
        { TOP - 1,             0,                1,          1 },  // one byte below top, fits
        { TOP - 0x1000,        0,                0x1001,     0 },  // one byte past top
    };
    for (unsigned i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
        int got = elf_seg_range_ok(s[i].va, s[i].bias, s[i].msz, TOP);
        if (got != s[i].want) {
            kprintf("[elf_test] FAIL seg va=0x%lx bias=0x%lx msz=0x%lx got=%d want=%d\n",
                    (unsigned long)s[i].va, (unsigned long)s[i].bias,
                    (unsigned long)s[i].msz, got, s[i].want);
            fails++;
        }
    }

    kprintf(fails ? "[elf_test] SELF-TEST FAILED\n"
                  : "[elf_test] SELF-TEST PASSED (phdr table + segment range bounds)\n");
}
#endif

// ── Internal: load ELF into a fresh address space ─────────────────────────
// Allocates new PML4 + mm_t, maps PT_LOAD segments, sets up brk and stack VMA.
// Does NOT create a kstack or fd_table — those are for elf_load only.
// All out_* params except out_pml4/out_mm/out_entry may be NULL.
uint8_t elf_load_into(const uint8_t* data, uint64_t size,
                      phys_addr_t* out_pml4, mm_t** out_mm, uint64_t* out_entry,
                      uint64_t* out_phdr_vaddr,
                      uint16_t* out_phnum,
                      uint16_t* out_phent,
                      vfs_file_t* backing_file) {
    if (!data || size < sizeof(Elf64_Ehdr)) return 0;

    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;

    // Validate ELF magic.
    uint32_t magic = (uint32_t)ehdr->e_ident[0]
                   | ((uint32_t)ehdr->e_ident[1] << 8)
                   | ((uint32_t)ehdr->e_ident[2] << 16)
                   | ((uint32_t)ehdr->e_ident[3] << 24);
    if (magic != ELF_MAGIC)         return 0;
    if (ehdr->e_ident[4] != 2)      return 0; // ELFCLASS64
    // Accept ET_EXEC (static) and ET_DYN (static PIE).
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return 0;
    if (ehdr->e_machine != EM_X86_64) return 0;
    if (ehdr->e_phnum   == 0)        return 0;
    // Overflow-safe bounds check (see elf_phtab_in_bounds): a malicious e_phoff
    // near UINT64_MAX would otherwise wrap past this guard -> OOB phdr reads.
    if (!elf_phtab_in_bounds(ehdr->e_phoff, ehdr->e_phnum, size)) return 0;

    // For ET_DYN (static PIE), compute a load bias so the binary does not
    // collide with the kernel or other fixed mappings.
    // We place PIE at VMM_USER_CODE_BASE (4 MiB).  The ELF's p_vaddr values
    // are relative offsets from 0; we add the bias to get the final VA.
    virt_addr_t load_bias = (ehdr->e_type == ET_DYN) ? VMM_USER_CODE_BASE : 0;

    // Compute phdr table virtual address for AT_PHDR.
    // Preferred: use the PT_PHDR segment if the binary includes one.
    // Fallback: the phdr table sits at file offset e_phoff; for ET_EXEC
    // it lives inside the first PT_LOAD segment so we can derive its VA
    // by finding that segment.
    if (out_phdr_vaddr || out_phnum || out_phent) {
        uint64_t phdr_va = 0;
        // Scan for PT_PHDR first.
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            const Elf64_Phdr* ph = (const Elf64_Phdr*)(data + ehdr->e_phoff
                                                        + (uint64_t)i * sizeof(Elf64_Phdr));
            if (ph->p_type == 6 /*PT_PHDR*/) {
                phdr_va = ph->p_vaddr + load_bias;
                break;
            }
        }
        // If no PT_PHDR, fall back to first PT_LOAD that contains the headers.
        if (!phdr_va) {
            for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
                const Elf64_Phdr* ph = (const Elf64_Phdr*)(data + ehdr->e_phoff
                                                            + (uint64_t)i * sizeof(Elf64_Phdr));
                if (ph->p_type == PT_LOAD && ph->p_offset == 0 && ph->p_filesz >= ehdr->e_phoff) {
                    phdr_va = ph->p_vaddr + load_bias + ehdr->e_phoff;
                    break;
                }
            }
        }
        if (out_phdr_vaddr) *out_phdr_vaddr = phdr_va;
        if (out_phnum)      *out_phnum      = ehdr->e_phnum;
        if (out_phent)      *out_phent      = ehdr->e_phentsize;
    }

    // Allocate PML4.
    phys_addr_t pml4 = vmm_alloc_pml4();
    if (pml4 == PMM_INVALID_ADDR) return 0;

    // Create mm.
    mm_t* mm = mm_create();
    if (!mm) { pmm_buddy_free(pml4, 0); return 0; }

    virt_addr_t seg_end_max = 0;
    virt_addr_t seg_start_min = (virt_addr_t)-1;

    // Walk program headers.
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(data + ehdr->e_phoff
                                                    + (uint64_t)i * sizeof(Elf64_Phdr));
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        // Reject a segment whose [vaddr+bias, +memsz) overflows or escapes the
        // user half BEFORE deriving seg_start/seg_end (whose rounding can wrap
        // to seg_end < seg_start -> a bogus/empty VMA, or land in the kernel
        // half -> an un-faultable mapping).
        if (!elf_seg_range_ok(ph->p_vaddr, load_bias,
                              ph->p_memsz, VMM_USER_STACK_TOP)) {
            mm_destroy(mm);
            vmm_free_user(pml4);
            pmm_buddy_free(pml4, 0);
            return 0;
        }

        // Convert ELF flags to VMA flags.
        uint32_t prot_flags = 0;
        if (ph->p_flags & PF_R) prot_flags |= VMA_R;
        if (ph->p_flags & PF_W) prot_flags |= VMA_W;
        if (ph->p_flags & PF_X) prot_flags |= VMA_X;

        virt_addr_t seg_start = (ph->p_vaddr + load_bias) & ~PAGE_MASK;
        virt_addr_t seg_end   = (ph->p_vaddr + load_bias + ph->p_memsz + PAGE_MASK) & ~PAGE_MASK;

        if (backing_file) {
            // ── Lazy path: file-backed VMA, pages faulted in on demand ────
            // file_off: ELF file offset for the start of this page-aligned
            //   segment.  p_offset & PAGE_MASK == p_vaddr & PAGE_MASK (ELF
            //   alignment invariant), so rounding down to page is safe.
            uint64_t file_off = ph->p_offset & ~(uint64_t)PAGE_MASK;
            // file_len: bytes from file_off that are file-backed (rest = BSS).
            uint64_t file_len = (ph->p_offset & PAGE_MASK) + ph->p_filesz;
            if (!mm_vma_add_file(mm, seg_start, seg_end, prot_flags,
                                 backing_file, file_off, file_len)) {
                mm_destroy(mm);
                vmm_free_user(pml4);
                pmm_buddy_free(pml4, 0);
                return 0;
            }
        } else {
            // ── Eager path: allocate and populate all pages now ────────────
            uint32_t vma_flags = VMA_ANON | prot_flags;
            if (!mm_vma_add(mm, seg_start, seg_end, vma_flags)) {
                mm_destroy(mm);
                vmm_free_user(pml4);
                pmm_buddy_free(pml4, 0);
                return 0;
            }
            uint64_t pte_flags = mm_vma_pte_flags(vma_flags);
            for (virt_addr_t page = seg_start; page < seg_end; page += PAGE_SIZE) {
                phys_addr_t frame = pmm_buddy_alloc(0);
                if (frame == PMM_INVALID_ADDR) {
                    mm_destroy(mm);
                    vmm_free_user(pml4);
                    pmm_buddy_free(pml4, 0);
                    return 0;
                }
                uint8_t* fptr = (uint8_t*)(frame + HHDM_OFFSET);
                __builtin_memset(fptr, 0, PAGE_SIZE);

                virt_addr_t page_end      = page + PAGE_SIZE;
                virt_addr_t file_start_va = ph->p_vaddr + load_bias;
                virt_addr_t file_end_va   = ph->p_vaddr + load_bias + ph->p_filesz;
                if (file_start_va < page_end && file_end_va > page) {
                    virt_addr_t copy_start = (file_start_va > page) ? file_start_va : page;
                    virt_addr_t copy_end   = (file_end_va < page_end) ? file_end_va : page_end;
                    uint64_t    frame_off  = copy_start - page;
                    uint64_t    file_off   = ph->p_offset + (copy_start - file_start_va);
                    uint64_t    nbytes     = copy_end - copy_start;
                    if (file_off + nbytes <= size) {
                        __builtin_memcpy(fptr + frame_off, data + file_off, nbytes);
                    }
                }
                vmm_page_map(pml4, page, frame, pte_flags);
            }
        }

        if (seg_end > seg_end_max) seg_end_max = seg_end;
        if (seg_start < seg_start_min) seg_start_min = seg_start;
    }

    // ── AT_PHDR safety net ────────────────────────────────────────────────
    // If the phdr table isn't inside any PT_LOAD (binaries whose first
    // LOAD skips file offset 0 — e.g. foot/sway from meson), the loop
    // above left phdr_va == 0, so the program never finds its own program
    // headers.  Userland TLS setup reads PT_TLS from AT_PHDR to size the
    // %fs block; a zero AT_PHDR there sized every thread's TLS to ~0 and
    // every __thread access (errno, the pthread-key table) scribbled the
    // malloc heap.  Map a read-only page holding the ELF header + phdrs
    // just below the lowest segment and point AT_PHDR at it.
    if (out_phdr_vaddr && *out_phdr_vaddr == 0 && seg_start_min != (virt_addr_t)-1) {
        uint64_t hdr_bytes = ehdr->e_phoff
                           + (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
        if (hdr_bytes <= PAGE_SIZE && hdr_bytes <= size) {
            virt_addr_t hpage = seg_start_min - PAGE_SIZE;
            phys_addr_t frame = pmm_buddy_alloc(0);
            if (frame != PMM_INVALID_ADDR) {
                uint8_t* fptr = (uint8_t*)(frame + HHDM_OFFSET);
                __builtin_memset(fptr, 0, PAGE_SIZE);
                __builtin_memcpy(fptr, data, hdr_bytes);
                if (mm_vma_add(mm, hpage, hpage + PAGE_SIZE, VMA_R | VMA_ANON)) {
                    vmm_page_map(pml4, hpage, frame,
                                 mm_vma_pte_flags(VMA_R | VMA_ANON));
                    *out_phdr_vaddr = hpage + ehdr->e_phoff;
                } else {
                    pmm_buddy_free(frame, 0);
                }
            }
        }
    }

    // Heap: starts after last segment, page-aligned.
    virt_addr_t brk_start = (seg_end_max + PAGE_MASK) & ~PAGE_MASK;
    if (brk_start < VMM_USER_CODE_BASE) brk_start = VMM_USER_CODE_BASE + PAGE_SIZE;
    mm->brk_start = brk_start;
    mm->brk       = brk_start;

    // Stack VMA (demand-paged).
    virt_addr_t stack_end   = VMM_USER_STACK_TOP;
    virt_addr_t stack_start = stack_end - VMM_USER_STACK_PAGES * PAGE_SIZE;
    mm_vma_add(mm, stack_start, stack_end, VMA_R | VMA_W | VMA_ANON);

    *out_pml4  = pml4;
    *out_mm    = mm;
    *out_entry = ehdr->e_entry + load_bias;
    return 1;
}

// ── elf_setup_stack ────────────────────────────────────────────────────────
// Build the SysV AMD64 initial user stack.
//
// We allocate a flat kernel buffer, fill it with the correct layout, then
// copy it into the already-mapped user stack pages (which elf_load_into
// registered in the mm VMA).  No page-table walk needed.
//
// Layout (low → high, RSP points to lowest word):
//   argc          (uint64_t)
//   argv[0..n-1]  (uint64_t pointers into string area above)
//   NULL          (uint64_t 0)
//   envp[0..m-1]  (uint64_t pointers)
//   NULL          (uint64_t 0)
//   auxv pairs    (pairs of uint64_t: type, value)
//   AT_NULL pair  (0, 0)
//   [padding to align RSP % 16 == 0 before argc]
//   16-byte AT_RANDOM blob
//   envp strings  (NUL-terminated, packed)
//   argv strings  (NUL-terminated, packed)
//       ← VMM_USER_STACK_TOP
//
// The stack pages are already mapped (elf_load_into created the VMA and
// the demand-fault handler will fill them); we need to pre-populate the
// initial pages.  We do this by allocating frames ourselves for the top
// portion and mapping them into the new PML4.
//
// Returns the initial user RSP, or 0 on failure.

uint64_t elf_setup_stack(phys_addr_t pml4,
                          const char* const* argv, const char* const* envp,
                          uint64_t entry,
                          uint64_t phdr_vaddr, uint16_t phnum, uint16_t phent) {
    // ── Count and measure ─────────────────────────────────────────────────
    uint32_t argc = 0, envc = 0;
    if (argv) while (argv[argc]) argc++;
    if (envp) while (envp[envc]) envc++;

    // String bytes (argv strings + envp strings + NUL terminators).
    uint64_t str_bytes = 0;
    for (uint32_t i = 0; i < argc; i++) {
        uint64_t l = 0; while (argv[i][l]) l++; str_bytes += l + 1;
    }
    for (uint32_t i = 0; i < envc; i++) {
        uint64_t l = 0; while (envp[i][l]) l++; str_bytes += l + 1;
    }
    str_bytes += 16; // AT_RANDOM 16-byte blob

    // Auxv: 13 entries × 16 bytes = 208 bytes.
    // Entries: PHDR, PHENT, PHNUM, PAGESZ, ENTRY, UID, EUID, GID, EGID,
    //          HWCAP, CLKTCK, RANDOM, NULL.
    const uint32_t N_AUXV = 13;
    uint64_t ptr_words = 1            // argc
                       + argc + 1    // argv[] + NULL
                       + envc + 1    // envp[] + NULL
                       + N_AUXV * 2; // auxv (key,val) pairs
    uint64_t ptr_bytes = ptr_words * 8;

    // Total bytes needed (add 16 for alignment slack).
    uint64_t total = str_bytes + ptr_bytes + 16;

    // ── Allocate and pre-populate stack pages ─────────────────────────────
    // We allocate enough physical pages to hold `total` bytes just below
    // VMM_USER_STACK_TOP, map them into the new PML4, and return a kernel
    // pointer to write into.
    uint64_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;
    if (pages > VMM_USER_STACK_PAGES) return 0; // sanity

    virt_addr_t utop   = VMM_USER_STACK_TOP;
    virt_addr_t ubase  = utop - pages * PAGE_SIZE;

    // Allocate frames and map them; keep a kernel-side shadow buffer.
    uint8_t* kbuf = (uint8_t*)kmalloc(pages * PAGE_SIZE);
    if (!kbuf) return 0;
    // Zero the kernel shadow buffer.
    __builtin_memset(kbuf, 0, pages * PAGE_SIZE);

    // Map each page.
    for (uint64_t p = 0; p < pages; p++) {
        phys_addr_t frame = pmm_buddy_alloc(0);
        if (frame == PMM_INVALID_ADDR) { kfree(kbuf); return 0; }
        // Zero the physical frame.
        uint8_t* fp = (uint8_t*)(frame + HHDM_OFFSET);
        __builtin_memset(fp, 0, PAGE_SIZE);
        vmm_page_map(pml4, ubase + p * PAGE_SIZE, frame, VMM_UDATA);
    }

    // ── Build stack content in the kernel shadow buffer ───────────────────
    // The buffer covers [ubase, ubase + pages*PAGE_SIZE).
    // Offset within buffer = user_va - ubase.

    // Phase 1: write strings starting from the top of the buffer downward.
    uint64_t off = pages * PAGE_SIZE; // offset from kbuf base (exclusive)

    // AT_RANDOM blob.
    off -= 16;
    uint64_t at_random_va = ubase + off;
    {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t seed = ((uint64_t)hi << 32) | lo;
        for (int b = 0; b < 16; b++) {
            kbuf[off + (uint64_t)b] = (uint8_t)seed;
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        }
    }

    // Allocate user-VA pointer arrays in kernel heap.
    uint64_t* argv_va = (uint64_t*)kmalloc((argc + 1) * 8);
    uint64_t* envp_va = (uint64_t*)kmalloc((envc + 1) * 8);
    if (!argv_va || !envp_va) {
        if (argv_va) kfree(argv_va);
        if (envp_va) kfree(envp_va);
        kfree(kbuf);
        return 0;
    }

    // Write envp strings top-down (envp[0] nearest the pointer table).
    for (int32_t i = (int32_t)envc - 1; i >= 0; i--) {
        uint64_t l = 0; while (envp[i][l]) l++; l++;
        off -= l;
        __builtin_memcpy(kbuf + off, envp[i], l);
        envp_va[i] = ubase + off;
    }
    envp_va[envc] = 0;

    // Write argv strings top-down.
    for (int32_t i = (int32_t)argc - 1; i >= 0; i--) {
        uint64_t l = 0; while (argv[i][l]) l++; l++;
        off -= l;
        __builtin_memcpy(kbuf + off, argv[i], l);
        argv_va[i] = ubase + off;
    }
    argv_va[argc] = 0;

    // Phase 2: build the pointer table below the string area.
    // Compute RSP: enough room for ptr_words × 8 bytes, aligned to 16.
    uint64_t rsp = (ubase + off - ptr_bytes) & ~(uint64_t)0xF;
    if (rsp < ubase) { kfree(argv_va); kfree(envp_va); kfree(kbuf); return 0; }

    // Write words sequentially at rsp (user VA → kbuf offset).
    uint64_t woff = rsp - ubase; // write offset in kbuf

    // Macro: write one uint64_t word and advance woff.
    #define WWORD(v) do { \
        uint64_t _v = (uint64_t)(v); \
        kbuf[woff]   = (uint8_t)(_v);       kbuf[woff+1] = (uint8_t)(_v>>8);  \
        kbuf[woff+2] = (uint8_t)(_v>>16);   kbuf[woff+3] = (uint8_t)(_v>>24); \
        kbuf[woff+4] = (uint8_t)(_v>>32);   kbuf[woff+5] = (uint8_t)(_v>>40); \
        kbuf[woff+6] = (uint8_t)(_v>>48);   kbuf[woff+7] = (uint8_t)(_v>>56); \
        woff += 8; \
    } while (0)

    WWORD(argc);
    for (uint32_t i = 0; i < argc; i++) WWORD(argv_va[i]);
    WWORD(0); // argv NULL
    for (uint32_t i = 0; i < envc; i++) WWORD(envp_va[i]);
    WWORD(0); // envp NULL
    // auxv (key, value) pairs:
    WWORD(AT_PHDR);   WWORD(phdr_vaddr);
    WWORD(AT_PHENT);  WWORD(phent);
    WWORD(AT_PHNUM);  WWORD(phnum);
    WWORD(AT_PAGESZ); WWORD(PAGE_SIZE);
    WWORD(AT_ENTRY);  WWORD(entry);
    WWORD(AT_UID);    WWORD(0);
    WWORD(AT_EUID);   WWORD(0);
    WWORD(AT_GID);    WWORD(0);
    WWORD(AT_EGID);   WWORD(0);
    WWORD(AT_HWCAP);  WWORD(0);
    WWORD(AT_CLKTCK); WWORD(100);
    WWORD(AT_RANDOM); WWORD(at_random_va);
    WWORD(AT_NULL);   WWORD(0);

    #undef WWORD

    // Phase 3: flush the shadow buffer into the physical frames.
    // Each frame covers one page.  Copy the corresponding slice from kbuf.
    for (uint64_t p = 0; p < pages; p++) {
        // Find the frame mapped at ubase + p*PAGE_SIZE.
        // We just mapped it above but didn't save the frame address.
        // Walk the PML4 to recover it.
        uint64_t va = ubase + p * PAGE_SIZE;
        phys_addr_t frame = vmm_page_phys(pml4, va);
        if (frame == PMM_INVALID_ADDR) { kfree(argv_va); kfree(envp_va); kfree(kbuf); return 0; }
        uint8_t* dst = (uint8_t*)(frame + HHDM_OFFSET);
        const uint8_t* src = kbuf + p * PAGE_SIZE;
        __builtin_memcpy(dst, src, PAGE_SIZE);
    }

    kfree(argv_va);
    kfree(envp_va);
    kfree(kbuf);
    return rsp;
}

// ── stdio fd resolution ───────────────────────────────────────────────────
// Resolves a stdio[3] spec into a vfs_file_t* for fd index i.
//   spec[i] == -1  → dup from g_current's fd table (inherit)
//   spec[i] >= 0   → dup that specific parent fd
//   spec == NULL   → open tty0 (safe default for init process with no parent)
static vfs_file_t* resolve_stdio(const int* spec, int i) {
    if (!spec) return tty_open(0);
    int fd = spec[i];
    if (fd == -2) return vfs_null_open(); // explicit /dev/null

    // Caller is the parent (g_current) setting up the child's stdio.  Dup the
    // requested slot (`fd`, or `i` when fd==-1 means "inherit") under the
    // parent's fd-table lock: a sibling thread sharing the table can close()
    // the fd concurrently, and the old lock-free "check slot then vfs_dup it"
    // double-fetched the slot and could vfs_dup a file the sibling had just
    // freed (a use-after-free).  Under the lock a non-NULL slot is live, so the
    // single vfs_dup is safe; vfs_dup(NULL) returns NULL for an empty slot.
    task_files_t* tf = (g_current) ? g_current->files_shared : NULL;
    if (!tf) return tty_open(0);
    uint32_t slot = (fd == -1) ? (uint32_t)i : (uint32_t)fd;
    vfs_file_t* f = NULL;
    spin_lock(&tf->lock);
    if (slot < tf->ft->cap) f = vfs_dup(tf->ft->fd_table[slot]);
    spin_unlock(&tf->lock);
    return f ? f : tty_open(0);   // empty / out-of-range slot -> fall back to tty0
}

// ── elf_load ──────────────────────────────────────────────────────────────
// Full task creation: address space + kstack + fd_table.
// stdio: array of 3 fd specs (see resolve_stdio). Pass NULL to open tty0.
task_t* elf_load(const uint8_t* data, uint64_t size, uint32_t pid,
                 vfs_file_t* backing_file) {
    phys_addr_t pml4;
    mm_t*       mm;
    uint64_t    entry;

    if (!elf_load_into(data, size, &pml4, &mm, &entry,
                       NULL, NULL, NULL, backing_file)) return NULL;

    // Allocate task + shared resources.
    task_t* t = kmalloc(sizeof(task_t));
    if (!t) { mm_destroy(mm); vmm_free_user(pml4); pmm_buddy_free(pml4, 0); return NULL; }
    // Zero the freshly slab-allocated task_t up front.  kmalloc returns slab
    // poison, and these exec paths have historically drifted from process.c's
    // field-by-field init, leaving members as garbage (sigstate.handlers ->
    // force-kill; cleartid_addr -> wild futex write on exit; fs_base /
    // wake_pending / pf_disk / pf_cache).  Zeroing first makes any member not
    // explicitly set below default to 0; the explicit non-zero inits (mm,
    // files, pledge, ctx, ...) still run after and override.  Drift-proof.
    __builtin_memset(t, 0, sizeof(*t));

    task_mm_t* tmm = task_mm_alloc(pml4, mm);
    if (!tmm) { kfree(t); mm_destroy(mm); vmm_free_user(pml4); pmm_buddy_free(pml4, 0); return NULL; }

    task_files_t* files = task_files_alloc();
    if (!files) { kfree(t); task_mm_release(tmm); return NULL; }
    // fd_table_init OOM leaves files->ft == NULL; the fd_table[0..2] stores
    // below would then write through NULL (kernel #PF).  Release and fail.
    if (!fd_table_init(files, 4)) {
        task_files_release(files); kfree(t); task_mm_release(tmm); return NULL;
    }
    files->ft->fd_table[0] = tty_open(0); // elf_load: no caller stdio spec, default tty0
    files->ft->fd_table[1] = tty_open(0);
    files->ft->fd_table[2] = tty_open(0);

    t->pid              = pid;
    t->tgid             = pid;
    t->ppid             = 0;
    t->pgid             = pid;
    t->sid              = pid;
    t->flags            = 0;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->children         = NULL;
    t->child_next       = NULL;
    t->pg_prev = t->pg_next = NULL;
    t->tg_prev = t->tg_next = NULL;
    t->sid_prev = t->sid_next = NULL;
    t->home_cpu = 0;
    t->mm_shared        = tmm;
    t->files_shared     = files;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->sigstate.pending   = 0;
    t->sigstate.blocked   = 0;
    t->sigstate.sigframe_rsp = 0;
    // exec resets all caught signals to SIG_DFL (POSIX).  The task_t was
    // freshly allocated, so handlers[] is otherwise slab poison (0x5aa5..)
    // — leaving it makes signal_setup_frame read poison as a "handler" and
    // force-SIGKILL the process the moment ANY signal is delivered for an
    // unregistered slot (e.g. SIGCHLD when a child exits).  That was the
    // silent killer of sway/svcmgr/login behind the desktop presentation
    // stall: a compositor died as soon as a helper (swaybg, status cmd,
    // config `exec`) exited.  Fresh-zeroed pages masked it intermittently.
    __builtin_memset(t->sigstate.handlers, 0, sizeof(t->sigstate.handlers));
    // exec also resets these per-task fields, which the elf.c exec paths
    // previously left as slab garbage (a drift from process.c's task init):
    //   cleartid_addr — a garbage value makes exit write 0 to a wild futex
    //                   address (futex clear-on-exit) and wake on it;
    //   fs_base       — garbage %fs base until libc sets TLS;
    //   wake_pending  — garbage feeds the lost-wakeup scheduler guard;
    //   last_ran_cpu  — garbage CPU-affinity hint;
    //   pf_disk/cache — produced UINT_MAX "[pf] … (109% warm)" accounting.
    t->cleartid_addr = 0;
    t->fs_base       = 0;
    t->last_ran_cpu  = 0;
    t->wake_pending  = 0;
    t->pf_disk       = 0;
    t->pf_cache      = 0;
    t->signalfd_head      = NULL;
    t->drm_bytes_charged  = 0;
    t->drm_priority       = 0;
    t->umask            = 0022u;
    t->exit_code        = 0;
    t->sleep_until_ns   = 0;
    t->cwd = kmalloc(KPATH_MAX);
    if (t->cwd) { t->cwd[0] = '/'; t->cwd[1] = '\0'; }
    // Credentials: inherit from parent if present, otherwise root.
    if (g_current && g_current->files_shared)
        cred_copy(&t->cred, &g_current->cred);
    else
        cred_init_root(&t->cred);
    __asm__ volatile("fxsave %0" : "=m"(t->ctx.fxsave_buf));

    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;

    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;
    *(--stk) = (uint64_t)user_trampoline;
    t->comm[0] = '\0'; // elf_load: no argv — comm unknown

    // exec: reset pledge to PLEDGE_ALL and unveil to empty (full visibility).
    // The new image sets its own restrictions.
    t->pledge_mask = PLEDGE_ALL;
    unveil_init(&t->unveil);

    *(--stk) = 0;                  // rbx
    *(--stk) = 0;                  // rbp
    *(--stk) = entry;              // r12 = user RIP
    *(--stk) = VMM_USER_STACK_TOP; // r13 = user RSP (no argv yet — elf_load path)
    *(--stk) = 0;                  // r14
    *(--stk) = 0;                  // r15
    t->ctx.rsp = (uint64_t)stk;

    return t;
}

// ── elf_load_with_argv ────────────────────────────────────────────────────
// Like elf_load but also calls elf_setup_stack to build the SysV stack.
// Required for Linux ABI binaries (bash, musl apps) that read argc/argv/envp.
//
// stdio: array of 3 fd specs resolved via resolve_stdio().
//   -1  = inherit the matching fd from the calling process (fork-like)
//   >=0 = dup that specific fd from the calling process
//   NULL = open tty0 for all three (safe for init process with no parent)
task_t* elf_load_with_argv(const uint8_t* data, uint64_t size, uint32_t pid,
                           const char* const* argv, const char* const* envp,
                           const int stdio[3], vfs_file_t* backing_file) {
    phys_addr_t pml4;
    mm_t*       mm;
    uint64_t    entry;
    uint64_t phdr_vaddr = 0;
    uint16_t phnum = 0, phent = 0;
    if (!elf_load_into(data, size, &pml4, &mm, &entry,
                       &phdr_vaddr, &phnum, &phent, backing_file)) return NULL;

    // Build the initial user stack with argc/argv/envp/auxv.
    uint64_t user_rsp = elf_setup_stack(pml4, argv, envp, entry,
                                         phdr_vaddr, phnum, phent);
    if (!user_rsp) {
        mm_destroy(mm);
        vmm_free_user(pml4);
        pmm_buddy_free(pml4, 0);
        return NULL;
    }

    // Allocate task + shared resources (same as elf_load).
    task_t* t = kmalloc(sizeof(task_t));
    if (!t) { mm_destroy(mm); vmm_free_user(pml4); pmm_buddy_free(pml4, 0); return NULL; }
    // Zero the freshly slab-allocated task_t up front.  kmalloc returns slab
    // poison, and these exec paths have historically drifted from process.c's
    // field-by-field init, leaving members as garbage (sigstate.handlers ->
    // force-kill; cleartid_addr -> wild futex write on exit; fs_base /
    // wake_pending / pf_disk / pf_cache).  Zeroing first makes any member not
    // explicitly set below default to 0; the explicit non-zero inits (mm,
    // files, pledge, ctx, ...) still run after and override.  Drift-proof.
    __builtin_memset(t, 0, sizeof(*t));

    task_mm_t* tmm = task_mm_alloc(pml4, mm);
    if (!tmm) { kfree(t); mm_destroy(mm); vmm_free_user(pml4); pmm_buddy_free(pml4, 0); return NULL; }

    task_files_t* files = task_files_alloc();
    if (!files) { kfree(t); task_mm_release(tmm); return NULL; }
    // fd_table_init OOM leaves files->ft == NULL; the fd_table[0..2] stores
    // below would then write through NULL (kernel #PF).  Release and fail.
    if (!fd_table_init(files, 4)) {
        task_files_release(files); kfree(t); task_mm_release(tmm); return NULL;
    }
    files->ft->fd_table[0] = resolve_stdio(stdio, 0);
    files->ft->fd_table[1] = resolve_stdio(stdio, 1);
    files->ft->fd_table[2] = resolve_stdio(stdio, 2);

    t->pid              = pid;
    t->tgid             = pid;
    t->ppid             = 0;
    t->pgid             = pid;
    t->sid              = pid;
    t->flags            = 0;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->children         = NULL;
    t->child_next       = NULL;
    t->pg_prev = t->pg_next = NULL;
    t->tg_prev = t->tg_next = NULL;
    t->sid_prev = t->sid_next = NULL;
    t->home_cpu = 0;
    t->mm_shared        = tmm;
    t->files_shared     = files;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->sigstate.pending   = 0;
    t->sigstate.blocked   = 0;
    t->sigstate.sigframe_rsp = 0;
    // exec resets all caught signals to SIG_DFL (POSIX).  The task_t was
    // freshly allocated, so handlers[] is otherwise slab poison (0x5aa5..)
    // — leaving it makes signal_setup_frame read poison as a "handler" and
    // force-SIGKILL the process the moment ANY signal is delivered for an
    // unregistered slot (e.g. SIGCHLD when a child exits).  That was the
    // silent killer of sway/svcmgr/login behind the desktop presentation
    // stall: a compositor died as soon as a helper (swaybg, status cmd,
    // config `exec`) exited.  Fresh-zeroed pages masked it intermittently.
    __builtin_memset(t->sigstate.handlers, 0, sizeof(t->sigstate.handlers));
    // exec also resets these per-task fields, which the elf.c exec paths
    // previously left as slab garbage (a drift from process.c's task init):
    //   cleartid_addr — a garbage value makes exit write 0 to a wild futex
    //                   address (futex clear-on-exit) and wake on it;
    //   fs_base       — garbage %fs base until libc sets TLS;
    //   wake_pending  — garbage feeds the lost-wakeup scheduler guard;
    //   last_ran_cpu  — garbage CPU-affinity hint;
    //   pf_disk/cache — produced UINT_MAX "[pf] … (109% warm)" accounting.
    t->cleartid_addr = 0;
    t->fs_base       = 0;
    t->last_ran_cpu  = 0;
    t->wake_pending  = 0;
    t->pf_disk       = 0;
    t->pf_cache      = 0;
    t->signalfd_head      = NULL;
    t->drm_bytes_charged  = 0;
    t->drm_priority       = 0;
    t->umask            = 0022u;
    t->exit_code        = 0;
    t->sleep_until_ns   = 0;
    t->cwd = kmalloc(KPATH_MAX);
    if (t->cwd) { t->cwd[0] = '/'; t->cwd[1] = '\0'; }
    // Credentials: inherit from spawning process if present, otherwise root.
    if (g_current && g_current->files_shared)
        cred_copy(&t->cred, &g_current->cred);
    else
        cred_init_root(&t->cred);
    __asm__ volatile("fxsave %0" : "=m"(t->ctx.fxsave_buf));

    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;

    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;
    *(--stk) = (uint64_t)user_trampoline;
    // Set comm: basename of argv[0], max 15 chars.
    {
        const char* name = (argv && argv[0]) ? argv[0] : "unknown";
        const char* base = name;
        for (const char* p = name; *p; p++) if (*p == '/') base = p + 1;
        uint32_t ci = 0;
        while (ci < 15 && base[ci]) { t->comm[ci] = base[ci]; ci++; }
        t->comm[ci] = '\0';
    }

    // exec: reset pledge to PLEDGE_ALL and unveil to empty.
    // Credentials are inherited (set above); pledge/unveil reset on exec
    // so the new image can set its own restrictions.
    t->pledge_mask = PLEDGE_ALL;
    unveil_init(&t->unveil);

    *(--stk) = 0;          // rbx
    *(--stk) = 0;          // rbp
    *(--stk) = entry;      // r12 = user RIP
    *(--stk) = user_rsp;   // r13 = user RSP (points to argc on stack)
    *(--stk) = 0;          // r14
    *(--stk) = 0;          // r15
    t->ctx.rsp = (uint64_t)stk;

    return t;
}

// ── elf_read_buf ──────────────────────────────────────────────────────────
// Shared read primitive used by every ELF load path (spawn, exec, kernel).
//
// Seeks f to EOF to get the exact file size, allocates a precisely-sized
// kmalloc buffer, reads all bytes, and logs the read timing to serial.
// Returns the buffer (caller must kfree) and fills *out_size, *out_t0,
// *out_t1 for map-phase timing downstream.  Returns NULL on any failure.
//
// 32 MiB hard cap guards against malformed or hostile binaries exhausting
// kernel heap before lazy-load / demand-paging is implemented.
#define ELF_LOAD_MAX (32ULL * 1024ULL * 1024ULL)

uint8_t* elf_read_buf(vfs_file_t* f, int64_t* out_size,
                       uint64_t* out_t0, uint64_t* out_t1) {
    if (!f || !f->seek) return NULL;
    int64_t file_size = f->seek(f, 0, 2 /*SEEK_END*/);
    if (file_size <= 0 || (uint64_t)file_size > ELF_LOAD_MAX) return NULL;
    f->seek(f, 0, 0 /*SEEK_SET*/);

    uint8_t* buf = (uint8_t*)kmalloc((uint64_t)file_size);
    if (!buf) return NULL;

    uint64_t t0 = tsc_read_ns();
    int64_t n = vfs_read(f, buf, (uint64_t)file_size);
    uint64_t t1 = tsc_read_ns();
    kprintf("[elf] read %s: %u bytes in %u us\n",
            f->path[0] ? f->path : "?",
            (uint32_t)file_size,
            (uint32_t)((t1 - t0) / 1000u));

    if (n != file_size) { kfree(buf); return NULL; }

    *out_size = file_size;
    *out_t0   = t0;
    *out_t1   = t1;
    return buf;
}

static task_t* elf_load_vfs(vfs_file_t* f, uint32_t pid) {
    uint8_t* hdr = (uint8_t*)kmalloc(PAGE_SIZE);
    if (!hdr) return NULL;
    f->seek(f, 0, 0);
    uint64_t t0 = tsc_read_ns();
    int64_t n = vfs_read(f, hdr, PAGE_SIZE);
    uint64_t t1 = tsc_read_ns();
    if (n < (int64_t)sizeof(Elf64_Ehdr)) { kfree(hdr); return NULL; }
    kprintf("[elf] lazy %s: header %u us (demand-paged)\n",
            f->path[0] ? f->path : "?",
            (uint32_t)((t1 - t0) / 1000u));
    task_t* t = elf_load(hdr, (uint64_t)n, pid, f);
    kfree(hdr);
    return t;
}

static task_t* elf_load_vfs_with_argv(vfs_file_t* f, uint32_t pid,
                                       const char* const* argv,
                                       const char* const* envp,
                                       const int stdio[3]) {
    uint8_t* hdr = (uint8_t*)kmalloc(PAGE_SIZE);
    if (!hdr) return NULL;
    f->seek(f, 0, 0);
    uint64_t t0 = tsc_read_ns();
    int64_t n = vfs_read(f, hdr, PAGE_SIZE);
    uint64_t t1 = tsc_read_ns();
    if (n < (int64_t)sizeof(Elf64_Ehdr)) { kfree(hdr); return NULL; }
    kprintf("[elf] lazy %s: header %u us (demand-paged)\n",
            f->path[0] ? f->path : "?",
            (uint32_t)((t1 - t0) / 1000u));
    task_t* t = elf_load_with_argv(hdr, (uint64_t)n, pid, argv, envp, stdio, f);
    kfree(hdr);
    return t;
}

// ── elf_exec_from_ext2 ────────────────────────────────────────────────────
task_t* elf_exec_from_ext2(const char* path, uint32_t pid,
                            const char* const* argv, const char* const* envp,
                            const int stdio[3]) {
    // ── Execute permission check (path-aware walk + exec bit) ────────────
    // exec_ino is declared outside the block so it survives into the open
    // call below — eliminating the redundant second path walk.
    uint32_t exec_ino;
    // Hoisted so the setuid escalation can be applied to the child after it is
    // built (and the requester's identity captured for the ksec request).
    uint32_t setuid_uid   = 0xFFFFFFFFu;   // 0xFFFFFFFF = no escalation
    uint32_t caller_uid   = 0;
    uint32_t caller_gid   = 0;
    {
        cred_t root_cred; cred_init_root(&root_cred);
        const cred_t* c = (g_current && g_current->files_shared)
                          ? &g_current->cred : &root_cred;
        caller_uid = c->euid;
        caller_gid = c->egid;
        int exec_err = 0;
        exec_ino = ext2_lookup_path(path, c, &exec_err);
        if (!exec_ino) return NULL;
        ext2_inode_t exec_inode;
        if (!ext2_read_inode(exec_ino, &exec_inode)) return NULL;
        inode_perm_t ip = {
            .uid = exec_inode.i_uid, .gid = exec_inode.i_gid,
            // Full low-12 mode so vfs_check_exec sees the setuid bit (04000);
            // acl_from_mode only reads the low 9, so the perm check is unaffected.
            .mode = exec_inode.i_mode & 0xFFF,
            .inode_nr = exec_ino, .dev = 0, .nosuid = 0,
        };
        if (vfs_check_exec(&ip, c, &setuid_uid) != 0) return NULL;
    }

    // Open by the already-resolved inode — no second path walk.
    vfs_file_t* f = ext2_open_ino(exec_ino, path);
    if (!f) return NULL;

    task_t* t = elf_load_vfs_with_argv(f, pid, argv, envp, stdio);
    vfs_close(f);
    // setuid-on-exec: apply any escalation to the new (not-yet-scheduled) child,
    // ksec-mediated and fail-closed (no agent / deny -> no change).
    if (t) ksec_exec_setuid(&t->cred, setuid_uid, exec_ino,
                            (g_current ? g_current->pid : 0),
                            caller_uid, caller_gid);
    return t;
}

// ── elf_exec_kernel ───────────────────────────────────────────────────────
// Kernel-internal exec: trusted path, no permission check, no cred.
// Used by init_kthread to launch the first userspace processes, which are
// compiled-in trusted binaries — checking permissions against root_cred
// is correct but wasteful busywork when the kernel is its own authority.
task_t* elf_exec_kernel(const char* path, uint32_t pid,
                         const char* const* argv, const char* const* envp,
                         const int stdio[3]) {
    uint32_t ino = ext2_lookup_path_raw(path);
    if (!ino) return NULL;
    vfs_file_t* f = ext2_open_ino(ino, path);
    if (!f) return NULL;
    task_t* t = elf_load_vfs_with_argv(f, pid, argv, envp, stdio);
    vfs_close(f);
    return t;
}

// ── elf_load_from_ext2 ────────────────────────────────────────────────────
task_t* elf_load_from_ext2(const char* path, uint32_t pid) {
    vfs_file_t* f = ext2_open(path);
    if (!f) return NULL;
    task_t* t = elf_load_vfs(f, pid);
    vfs_close(f);
    return t;
}
