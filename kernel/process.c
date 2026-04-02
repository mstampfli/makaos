#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "kheap.h"
#include "vfs.h"

// ── task_create_kthread ───────────────────────────────────────────────────
// Kernel thread: shares the kernel PML4 (no clone), runs at CPL=0.
// kstack allocated BEFORE pml4 so the shallow-copy in vmm_alloc_pml4
// already includes this stack's mapping in the upper half.
task_t* task_create_kthread(void (*entry)(void), uint32_t pid) {
    task_t* t = kmalloc(sizeof(task_t));

    t->pid              = pid;
    t->tgid             = pid;
    t->ppid             = 0;
    t->flags            = TASK_FLAG_KTHREAD;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->mm               = NULL;
    t->sigstate.pending = 0;
    t->sigstate.blocked = 0;

    // Allocate kstack first, then PML4 (so PML4 copy includes this stack).
    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;
    t->pml4_phys  = vmm_alloc_pml4();

    // Build initial kernel stack frame for context_switch.
    // context_switch pops: r15, r14, r13, r12, rbp, rbx  then ret.
    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;                  // alignment pad
    *(--stk) = (uint64_t)proc_trampoline;
    *(--stk) = 0;                  // rbx
    *(--stk) = 0;                  // rbp
    *(--stk) = (uint64_t)entry;    // r12 → picked up by proc_trampoline
    *(--stk) = 0;                  // r13
    *(--stk) = 0;                  // r14
    *(--stk) = 0;                  // r15

    t->ctx.rsp = (uint64_t)stk;

    // Set up standard file descriptors.
    for (int i = 0; i < VFS_MAX_FDS; i++) t->fd_table[i] = NULL;
    t->fd_table[0] = vfs_kbd_open();
    t->fd_table[1] = vfs_vga_open();
    t->fd_table[2] = vfs_vga_open();

    return t;
}

// ── task_create_user ──────────────────────────────────────────────────────
// User process: own PML4 + mm_t, ring-3 entry via user_trampoline + iretq.
// Code pages are mapped eagerly (they're already loaded into physical memory).
// Stack and heap are demand-paged: VMAs registered here, physical frames
// allocated on first #PF.
task_t* task_create_user(phys_addr_t code_phys, uint32_t code_pages, uint32_t pid) {
    task_t* t = kmalloc(sizeof(task_t));

    t->pid              = pid;
    t->tgid             = pid;
    t->ppid             = 0;
    t->flags            = 0;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->sigstate.pending = 0;
    t->sigstate.blocked = 0;

    // Allocate kstack first, then PML4.
    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;
    t->pml4_phys  = vmm_alloc_pml4();

    // Create the memory descriptor.
    mm_t* mm = mm_create();
    t->mm = mm;

    // ── Code region: read+execute, no write (W^X) ────────────────────────
    virt_addr_t code_start = VMM_USER_CODE_BASE;
    virt_addr_t code_end   = code_start + (virt_addr_t)code_pages * PAGE_SIZE;
    mm_vma_add(mm, code_start, code_end, VMA_R | VMA_X);

    // Map code pages eagerly — they're already in physical memory.
    for (uint32_t i = 0; i < code_pages; i++) {
        vmm_page_map(t->pml4_phys,
                     code_start + (virt_addr_t)i * PAGE_SIZE,
                     code_phys  + (phys_addr_t)i * PAGE_SIZE,
                     mm_vma_pte_flags(VMA_R | VMA_X));
    }

    // ── Stack region: read+write, no execute ─────────────────────────────
    // Guard page: one unmapped page just below the stack — no VMA there,
    // so any access faults and kills the process (stack overflow detection).
    virt_addr_t stack_end   = VMM_USER_STACK_TOP;
    virt_addr_t stack_start = stack_end - VMM_USER_STACK_PAGES * PAGE_SIZE;
    mm_vma_add(mm, stack_start, stack_end, VMA_R | VMA_W | VMA_ANON);
    // Stack is demand-paged: no physical frames allocated here.
    // The first push from user_trampoline will fault → page in.

    // ── Heap region: starts just after code, demand-paged ────────────────
    virt_addr_t heap_start = (code_end + PAGE_SIZE - 1) & ~PAGE_MASK; // page-align up
    mm->brk_start = heap_start;
    mm->brk       = heap_start;
    // No VMA yet — sys_brk adds one when the process calls brk().

    // Build kernel stack frame: context_switch → user_trampoline → iretq.
    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;                          // alignment
    *(--stk) = (uint64_t)user_trampoline;  // ret addr
    *(--stk) = 0;                          // rbx
    *(--stk) = 0;                          // rbp
    *(--stk) = VMM_USER_CODE_BASE;         // r12 = user RIP
    *(--stk) = VMM_USER_STACK_TOP;         // r13 = user RSP
    *(--stk) = 0;                          // r14
    *(--stk) = 0;                          // r15

    t->ctx.rsp = (uint64_t)stk;

    // Set up standard file descriptors.
    for (int i = 0; i < VFS_MAX_FDS; i++) t->fd_table[i] = NULL;
    t->fd_table[0] = vfs_kbd_open();
    t->fd_table[1] = vfs_vga_open();
    t->fd_table[2] = vfs_vga_open();

    return t;
}

// ── task_get_mm ───────────────────────────────────────────────────────────
mm_t* task_get_mm(void* task) {
    return ((task_t*)task)->mm;
}

// ── task_destroy ──────────────────────────────────────────────────────────
void task_destroy(task_t* t) {
    if (!t) return;
    kstack_free(t->kstack_top);
    // vmm_free_user frees all page table frames AND the physical frames
    // that were demand-paged into the lower half.
    vmm_free_user(t->pml4_phys);
    pmm_buddy_free(t->pml4_phys, 0);
    // Free VMA descriptors (mm_destroy does not touch physical memory).
    if (t->mm) mm_destroy(t->mm);
    kfree(t);
}
