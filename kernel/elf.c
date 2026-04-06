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

// ── Internal: load ELF into a fresh address space ─────────────────────────
// Allocates new PML4 + mm_t, maps PT_LOAD segments, sets up brk and stack VMA.
// Does NOT create a kstack or fd_table — those are for elf_load only.
uint8_t elf_load_into(const uint8_t* data, uint64_t size,
                      phys_addr_t* out_pml4, mm_t** out_mm, uint64_t* out_entry) {
    if (!data || size < sizeof(Elf64_Ehdr)) return 0;

    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;

    // Validate ELF magic.
    uint32_t magic = (uint32_t)ehdr->e_ident[0]
                   | ((uint32_t)ehdr->e_ident[1] << 8)
                   | ((uint32_t)ehdr->e_ident[2] << 16)
                   | ((uint32_t)ehdr->e_ident[3] << 24);
    if (magic != ELF_MAGIC)         return 0;
    if (ehdr->e_ident[4] != 2)      return 0; // ELFCLASS64
    if (ehdr->e_type    != ET_EXEC)  return 0;
    if (ehdr->e_machine != EM_X86_64) return 0;
    if (ehdr->e_phnum   == 0)        return 0;
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr) > size) return 0;

    // Allocate PML4.
    phys_addr_t pml4 = vmm_alloc_pml4();
    if (pml4 == PMM_INVALID_ADDR) return 0;

    // Create mm.
    mm_t* mm = mm_create();
    if (!mm) { pmm_buddy_free(pml4, 0); return 0; }

    virt_addr_t seg_end_max = 0;

    // Walk program headers.
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(data + ehdr->e_phoff
                                                    + (uint64_t)i * sizeof(Elf64_Phdr));
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        // Convert ELF flags to VMA flags.
        uint32_t vma_flags = VMA_ANON;
        if (ph->p_flags & PF_R) vma_flags |= VMA_R;
        if (ph->p_flags & PF_W) vma_flags |= VMA_W;
        if (ph->p_flags & PF_X) vma_flags |= VMA_X;

        virt_addr_t seg_start = ph->p_vaddr & ~PAGE_MASK;
        virt_addr_t seg_end   = (ph->p_vaddr + ph->p_memsz + PAGE_MASK) & ~PAGE_MASK;

        if (!mm_vma_add(mm, seg_start, seg_end, vma_flags)) {
            mm_destroy(mm);
            vmm_free_user(pml4);
            pmm_buddy_free(pml4, 0);
            return 0;
        }

        // Map and populate pages eagerly (file data + zero BSS).
        uint64_t pte_flags = mm_vma_pte_flags(vma_flags);

        for (virt_addr_t page = seg_start; page < seg_end; page += PAGE_SIZE) {
            phys_addr_t frame = pmm_buddy_alloc(0);
            if (frame == PMM_INVALID_ADDR) {
                mm_destroy(mm);
                vmm_free_user(pml4);
                pmm_buddy_free(pml4, 0);
                return 0;
            }

            // Zero the frame.
            uint8_t* fptr = (uint8_t*)(frame + HHDM_OFFSET);
            for (int j = 0; j < (int)PAGE_SIZE; j++) fptr[j] = 0;

            // Copy file data that falls in this page.
            virt_addr_t page_end = page + PAGE_SIZE;
            // File data range: [p_vaddr, p_vaddr + p_filesz)
            virt_addr_t file_start_va = ph->p_vaddr;
            virt_addr_t file_end_va   = ph->p_vaddr + ph->p_filesz;

            if (file_start_va < page_end && file_end_va > page) {
                virt_addr_t copy_start = (file_start_va > page) ? file_start_va : page;
                virt_addr_t copy_end   = (file_end_va < page_end) ? file_end_va : page_end;
                uint64_t    frame_off  = copy_start - page;
                uint64_t    file_off   = ph->p_offset + (copy_start - ph->p_vaddr);
                uint64_t    nbytes     = copy_end - copy_start;

                if (file_off + nbytes <= size) {
                    const uint8_t* src = data + file_off;
                    uint8_t* dst = fptr + frame_off;
                    for (uint64_t b = 0; b < nbytes; b++) dst[b] = src[b];
                }
            }

            vmm_page_map(pml4, page, frame, pte_flags);
        }

        if (seg_end > seg_end_max) seg_end_max = seg_end;
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
    *out_entry = ehdr->e_entry;
    return 1;
}

// ── elf_load ──────────────────────────────────────────────────────────────
// Full task creation: address space + kstack + fd_table.
task_t* elf_load(const uint8_t* data, uint64_t size, uint32_t pid) {
    phys_addr_t pml4;
    mm_t*       mm;
    uint64_t    entry;

    if (!elf_load_into(data, size, &pml4, &mm, &entry)) return NULL;

    // Allocate task + shared resources.
    task_t* t = kmalloc(sizeof(task_t));
    if (!t) { mm_destroy(mm); vmm_free_user(pml4); pmm_buddy_free(pml4, 0); return NULL; }

    task_mm_t* tmm = task_mm_alloc(pml4, mm);
    if (!tmm) { kfree(t); mm_destroy(mm); vmm_free_user(pml4); pmm_buddy_free(pml4, 0); return NULL; }

    task_files_t* files = task_files_alloc();
    if (!files) { kfree(t); task_mm_release(tmm); return NULL; }
    fd_table_init(files, 4);
    files->fd_table[0] = vfs_kbd_open();
    files->fd_table[1] = vfs_vga_open();
    files->fd_table[2] = vfs_vga_open();

    t->pid              = pid;
    t->tgid             = pid;
    t->ppid             = 0;
    t->flags            = 0;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->mm_shared        = tmm;
    t->files_shared     = files;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->sigstate.head    = 0;
    t->sigstate.tail    = 0;
    t->sigstate.blocked = 0;
    t->cwd[0] = '/';
    t->cwd[1] = '\0';

    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;

    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;
    *(--stk) = (uint64_t)user_trampoline;
    *(--stk) = 0;                  // rbx
    *(--stk) = 0;                  // rbp
    *(--stk) = entry;              // r12 = user RIP
    *(--stk) = VMM_USER_STACK_TOP; // r13 = user RSP
    *(--stk) = 0;                  // r14
    *(--stk) = 0;                  // r15
    t->ctx.rsp = (uint64_t)stk;

    return t;
}

// ── elf_load_from_ext2 ────────────────────────────────────────────────────
// Read the file at `path` from the ext2 filesystem into a kernel buffer,
// then call elf_load.
task_t* elf_load_from_ext2(const char* path, uint32_t pid) {
    vfs_file_t* f = ext2_open(path);
    if (!f) return NULL;

    // Read up to 1 MiB.
    const uint64_t MAX_ELF = 1024ULL * 1024ULL;
    uint8_t* buf = kmalloc((uint64_t)MAX_ELF);
    if (!buf) { vfs_close(f); return NULL; }

    int64_t n = vfs_read(f, buf, MAX_ELF);
    vfs_close(f);

    if (n <= 0) { kfree(buf); return NULL; }

    task_t* t = elf_load(buf, (uint64_t)n, pid);
    kfree(buf);
    return t;
}
