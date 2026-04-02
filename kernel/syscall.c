#include "syscall.h"
#include "common.h"
#include "sched.h"
#include "signal.h"
#include "process.h"
#include "mm.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "vfs.h"
#include "fat32.h"
#include "elf.h"
#include "tsc.h"

// g_vga is defined in idt.c — also used by vfs.c.
extern volatile uint16_t* g_vga;

// Scratch storage for the user RSP/RIP/RFLAGS during syscall entry.
uint64_t g_syscall_user_rsp    = 0;
uint64_t g_syscall_user_rip    = 0;
uint64_t g_syscall_user_rflags = 0;

// Exec redirect globals (read by syscall_entry.asm after exec syscall).
uint8_t     g_exec_requested = 0;
uint64_t    g_exec_entry     = 0;
uint64_t    g_exec_rsp       = 0;
phys_addr_t g_exec_pml4      = 0;

// ── MSR helpers ───────────────────────────────────────────────────────────
#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_SFMASK  0xC0000084

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)(val & 0xFFFFFFFF)),
                     "d"((uint32_t)(val >> 32)));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// ── syscall_init ──────────────────────────────────────────────────────────
void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);
    wrmsr(MSR_STAR, ((uint64_t)0x0008 << 32) | ((uint64_t)0x0018 << 48));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, (1 << 9) | (1 << 10));
}

// ── sys_write ─────────────────────────────────────────────────────────────
static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!g_current || fd >= g_current->files_shared->fd_capacity) return (uint64_t)-1;
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-1;
    int64_t r = vfs_write(f, (const void*)buf, len);
    return (r < 0) ? (uint64_t)-1 : (uint64_t)r;
}

// ── sys_read ──────────────────────────────────────────────────────────────
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags) {
    (void)flags;
    if (!g_current || fd >= g_current->files_shared->fd_capacity) return (uint64_t)-1;
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-1;
    int64_t r = vfs_read(f, (void*)buf, len);
    return (r < 0) ? (uint64_t)-1 : (uint64_t)r;
}

// ── sys_open ──────────────────────────────────────────────────────────────
// open(path, pathlen) → fd or -1
// path: pointer to an 11-byte 8.3 name (uppercase, space-padded).
// Allocates the lowest free fd in the calling task's fd_table.
static uint64_t sys_open(uint64_t path, uint64_t pathlen) {
    if (!g_current) return (uint64_t)-1;
    // Need exactly 11 bytes for an 8.3 name.
    if (pathlen < 11) return (uint64_t)-1;

    const char* name = (const char*)path;
    vfs_file_t* f = fat32_open(name);
    if (!f) return (uint64_t)-1;

    // Find the lowest free fd (skip 0/1/2 which are stdin/stdout/stderr).
    // Grow the table if all slots are occupied.
    for (uint32_t fd = 3; ; fd++) {
        if (fd >= g_current->files_shared->fd_capacity) {
            if (!fd_table_grow(g_current->files_shared)) {
                vfs_close(f);
                return (uint64_t)-1;
            }
        }
        if (!g_current->files_shared->fd_table[fd]) {
            g_current->files_shared->fd_table[fd] = f;
            return (uint64_t)fd;
        }
    }
}

// ── sys_close ─────────────────────────────────────────────────────────────
// close(fd) → 0 or -1
static uint64_t sys_close(uint64_t fd) {
    if (!g_current) return (uint64_t)-1;
    if (fd < 3 || fd >= g_current->files_shared->fd_capacity) return (uint64_t)-1;  // protect 0/1/2
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-1;
    vfs_close(f);
    g_current->files_shared->fd_table[fd] = NULL;
    return 0;
}

// ── sys_brk ───────────────────────────────────────────────────────────────
// brk(new_brk): set the top of the heap to new_brk.
//   - new_brk == 0: return current brk (Linux convention for querying).
//   - new_brk < brk_start: reject.
//   - new_brk > brk: grow heap — add/extend VMA, pages are demand-paged.
//   - new_brk < brk: shrink heap — remove VMA pages above new_brk
//     and unmap+free them.
// Returns new brk on success, (uint64_t)-1 on error.
static uint64_t sys_brk(uint64_t new_brk_raw) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-1;

    mm_t* mm = g_current->mm_shared->mm;

    // Query current brk.
    if (new_brk_raw == 0) return mm->brk;

    virt_addr_t new_brk = new_brk_raw & ~PAGE_MASK; // page-align down

    // Reject below heap start.
    if (new_brk < mm->brk_start) return (uint64_t)-1;

    // Reject if new_brk would overlap with the stack region.
    // (Stack starts at VMM_USER_STACK_TOP - VMM_USER_STACK_PAGES * PAGE_SIZE)
    virt_addr_t stack_floor = VMM_USER_STACK_TOP - VMM_USER_STACK_PAGES * PAGE_SIZE;
    if (new_brk >= stack_floor) return (uint64_t)-1;

    virt_addr_t old_brk = mm->brk;

    if (new_brk == old_brk) return old_brk;

    if (new_brk > old_brk) {
        // ── Grow heap ─────────────────────────────────────────────────────
        // Extend or add a heap VMA.  Physical pages are NOT allocated here;
        // they come in on-demand via the #PF handler.
        virt_addr_t heap_vma_start = mm->brk_start;

        // Remove the old heap VMA if present, then re-add the enlarged one.
        // (Simpler than a resize operation on the sorted list.)
        vma_t* prev = NULL;
        vma_t* v = mm->vmas;
        while (v) {
            if (v->start == heap_vma_start) {
                // Remove from list.
                if (prev) prev->next = v->next;
                else       mm->vmas  = v->next;
                kfree(v);
                break;
            }
            prev = v;
            v = v->next;
        }
        // Add enlarged VMA.
        if (!mm_vma_add(mm, heap_vma_start, new_brk, VMA_R | VMA_W | VMA_ANON))
            return (uint64_t)-1;

        mm->brk = new_brk;
        return new_brk;
    }

    // ── Shrink heap ───────────────────────────────────────────────────────
    // Unmap and free physical frames in [new_brk, old_brk).
    phys_addr_t pml4 = vmm_current_pml4();
    for (virt_addr_t page = new_brk; page < old_brk; page += PAGE_SIZE) {
        phys_addr_t frame;
        if (vmm_page_unmap(pml4, page, &frame))
            pmm_buddy_free(frame, 0);
    }

    // Shrink or remove the heap VMA.
    virt_addr_t heap_vma_start = mm->brk_start;
    vma_t* prev = NULL;
    vma_t* v = mm->vmas;
    while (v) {
        if (v->start == heap_vma_start) {
            if (new_brk <= heap_vma_start) {
                // Remove entirely.
                if (prev) prev->next = v->next;
                else       mm->vmas  = v->next;
                kfree(v);
            } else {
                v->end = new_brk;
            }
            break;
        }
        prev = v;
        v = v->next;
    }

    mm->brk = new_brk;
    return new_brk;
}

// ── sys_exit ──────────────────────────────────────────────────────────────
static uint64_t sys_exit(uint64_t code) {
    (void)code;
    if (g_current) g_current->state = TASK_DEAD;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
    return 0;
}

// ── sys_kill ──────────────────────────────────────────────────────────────
// kill(pid, sig): send signal `sig` to the task with the given pid.
// pid > 0: send to the task with that pid.
// pid == 0: send to all tasks in the caller's thread group.
// pid == -1: broadcast to all tasks (except idle/kernel threads).
static uint64_t sys_kill(uint64_t pid_raw, uint64_t sig_raw) {
    int sig = (int)(int64_t)sig_raw;
    if (sig < 1 || sig >= NSIG) return (uint64_t)-1;

    int64_t pid = (int64_t)pid_raw;

    if (pid > 0) {
        // Find task by pid — check current then walk all queues.
        task_t* target = NULL;
        if (g_current && (int64_t)g_current->pid == pid)
            target = g_current;
        if (!target) {
            typedef struct { int64_t pid; task_t* result; } find_arg_t;
            void find_cb(task_t* t, void* data) {
                find_arg_t* a = (find_arg_t*)data;
                if (!a->result && (int64_t)t->pid == a->pid) a->result = t;
            }
            find_arg_t a = {pid, NULL};
            sched_for_each(find_cb, &a);
            target = a.result;
        }
        if (!target) return (uint64_t)-1;
        signal_send(target, sig);
        return 0;
    }

    if (pid == 0) {
        if (!g_current) return (uint64_t)-1;
        signal_send_group(g_current->tgid, sig);
        return 0;
    }

    // pid == -1: broadcast to all non-kernel tasks.
    if (g_current && !(g_current->flags & TASK_FLAG_KTHREAD))
        signal_send(g_current, sig);
    void bcast_cb(task_t* t, void* data) {
        (void)data;
        if (!(t->flags & TASK_FLAG_KTHREAD)) signal_send(t, sig);
    }
    sched_for_each(bcast_cb, NULL);
    return 0;
}

// ── sys_fork ──────────────────────────────────────────────────────────────
static uint64_t sys_fork(void) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-1;
    task_t* child = task_fork(g_current, g_syscall_user_rip,
                              g_syscall_user_rflags, g_syscall_user_rsp);
    if (!child) return (uint64_t)-1;
    sched_add(child);
    return (uint64_t)child->pid;
}

// ── helper: convert a filename to 8.3 format ──────────────────────────────
static void filename_to_83(const char* name, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    // Find last dot.
    int dot = -1;
    for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    // Name part (up to 8).
    int n = (dot >= 0) ? dot : 0;
    for (int i = 0; name[i] && i < n; i++) n = i + 1;
    if (dot < 0) {
        // no dot — whole name is the name part
        n = 0;
        for (int i = 0; name[i]; i++) n++;
    }
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    // Extension (up to 3).
    if (dot >= 0) {
        const char* ext = name + dot + 1;
        for (int i = 0; i < 3 && ext[i]; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + i] = c;
        }
    }
}

// ── sys_exec ──────────────────────────────────────────────────────────────
static uint64_t sys_exec(uint64_t name_ptr, uint64_t namelen) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-1;
    if (namelen == 0 || namelen > 64) return (uint64_t)-1;

    // Copy filename from user space.
    char name_buf[65];
    const char* uname = (const char*)name_ptr;
    uint64_t len = namelen < 64 ? namelen : 64;
    for (uint64_t i = 0; i < len; i++) name_buf[i] = uname[i];
    name_buf[len] = '\0';

    // Convert to 8.3.
    char name83[11];
    filename_to_83(name_buf, name83);

    // Load ELF from FAT32 into a new address space.
    phys_addr_t new_pml4;
    mm_t*       new_mm;
    uint64_t    entry;

    // Read the file.
    vfs_file_t* f = fat32_open(name83);
    if (!f) return (uint64_t)-1;

    const uint64_t MAX_ELF = 1024ULL * 1024ULL;
    uint8_t* buf = kmalloc(MAX_ELF);
    if (!buf) { vfs_close(f); return (uint64_t)-1; }

    int64_t n = vfs_read(f, buf, MAX_ELF);
    vfs_close(f);

    if (n <= 0) { kfree(buf); return (uint64_t)-1; }

    uint8_t ok = elf_load_into(buf, (uint64_t)n, &new_pml4, &new_mm, &entry);
    kfree(buf);

    if (!ok) return (uint64_t)-1;

    // Replace the current task's address space.
    // Free old address space.
    vmm_free_user(g_current->mm_shared->pml4_phys);
    pmm_buddy_free(g_current->mm_shared->pml4_phys, 0);
    mm_destroy(g_current->mm_shared->mm);

    g_current->mm_shared->pml4_phys = new_pml4;
    g_current->mm_shared->mm        = new_mm;

    // Set exec globals so syscall_entry.asm redirects sysretq.
    g_exec_entry     = entry;
    g_exec_rsp       = VMM_USER_STACK_TOP;
    g_exec_pml4      = new_pml4;
    g_exec_requested = 1;

    return 0;
}

// ── sys_spawn ─────────────────────────────────────────────────────────────
// spawn(path_ptr, pathlen) → child pid, or -1 on error.
// Loads an ELF from the given absolute ext2 path into a brand-new address
// space and schedules it.  Parent returns immediately with the child's pid.
static uint64_t sys_spawn(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current) return (uint64_t)-1;
    if (pathlen == 0 || pathlen > 255) return (uint64_t)-1;

    // Copy path from user space.
    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    uint32_t pid = pid_alloc();
    if (!pid) return (uint64_t)-1;

    task_t* child = elf_load_from_ext2(path, pid);
    if (!child) {
        pid_free(pid);
        return (uint64_t)-1;
    }

    sched_add(child);
    return (uint64_t)pid;
}

// ── sys_thread ────────────────────────────────────────────────────────────
// thread(entry_ptr, stack_top, flags) → tid, or -1 on error.
// flags: THREAD_SHARE_MM (1<<0) — share address space
//        THREAD_SHARE_FILES (1<<1) — share fd table
// The caller must allocate the thread's user stack and pass its top.
static uint64_t sys_thread(uint64_t entry_ptr, uint64_t stack_top, uint64_t flags) {
    if (!g_current) return (uint64_t)-1;

    task_t* t = kmalloc(sizeof(task_t));
    if (!t) return (uint64_t)-1;

    t->pid = pid_alloc();
    if (!t->pid) { kfree(t); return (uint64_t)-1; }

    // Address space: share or deep-copy.
    if (flags & THREAD_SHARE_MM) {
        t->mm_shared = g_current->mm_shared;
        t->mm_shared->refs++;
    } else {
        phys_addr_t new_pml4 = vmm_alloc_pml4();
        if (new_pml4 == PMM_INVALID_ADDR) { pid_free(t->pid); kfree(t); return (uint64_t)-1; }
        if (!vmm_clone_user(new_pml4, g_current->mm_shared->pml4_phys)) {
            vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0);
            pid_free(t->pid); kfree(t); return (uint64_t)-1;
        }
        mm_t* new_mm = g_current->mm_shared->mm ? mm_clone(g_current->mm_shared->mm) : NULL;
        task_mm_t* tmm = task_mm_alloc(new_pml4, new_mm);
        if (!tmm) {
            if (new_mm) mm_destroy(new_mm);
            vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0);
            pid_free(t->pid); kfree(t); return (uint64_t)-1;
        }
        t->mm_shared = tmm;
    }

    // File descriptors: share or copy.
    if (flags & THREAD_SHARE_FILES) {
        t->files_shared = g_current->files_shared;
        t->files_shared->refs++;
    } else {
        task_files_t* files = task_files_alloc();
        if (!files) { task_mm_release(t->mm_shared); pid_free(t->pid); kfree(t); return (uint64_t)-1; }
        fd_table_init(files, g_current->files_shared->fd_capacity);
        for (uint32_t i = 0; i < g_current->files_shared->fd_capacity; i++)
            files->fd_table[i] = g_current->files_shared->fd_table[i];
        t->files_shared = files;
    }

    t->tgid            = g_current->tgid;
    t->ppid            = g_current->ppid;
    t->flags           = TASK_FLAG_THREAD;
    t->state           = TASK_READY;
    t->next            = NULL;
    t->mlfq_level      = 0;
    t->mlfq_ticks_left = 0;
    t->sigstate.head   = 0;
    t->sigstate.tail   = 0;
    t->sigstate.blocked = 0;

    // Build initial kernel stack frame.
    // context_switch → user_trampoline → iretq to ring 3.
    // r12 = user RIP (entry_ptr), r13 = user RSP (stack_top).
    virt_addr_t kstack_top = kstack_alloc();
    t->kstack_top = kstack_top;

    uint64_t* stk = (uint64_t*)kstack_top;
    *(--stk) = 0;                         // alignment
    *(--stk) = (uint64_t)user_trampoline;
    *(--stk) = 0;                         // rbx
    *(--stk) = 0;                         // rbp
    *(--stk) = entry_ptr;                 // r12 = user RIP
    *(--stk) = stack_top;                 // r13 = user RSP
    *(--stk) = 0;                         // r14
    *(--stk) = 0;                         // r15

    t->ctx.rsp = (uint64_t)stk;

    sched_add(t);
    return (uint64_t)t->pid;
}

// ── sys_wait ──────────────────────────────────────────────────────────────
static uint64_t sys_wait(uint64_t pid) {
    sched_wait_pid((uint32_t)pid);
    return 0;
}

// ── sys_getpid ────────────────────────────────────────────────────────────
static uint64_t sys_getpid(void) {
    return g_current ? (uint64_t)g_current->pid : 0;
}

// ── sys_readdir ───────────────────────────────────────────────────────────
static uint64_t sys_readdir(uint64_t buf_ptr, uint64_t max_entries) {
    if (!buf_ptr || max_entries == 0) return 0;
    if (max_entries > 256) max_entries = 256;

    fat32_entry_t* kbuf = kmalloc(max_entries * sizeof(fat32_entry_t));
    if (!kbuf) return (uint64_t)-1;

    int count = fat32_readdir(kbuf, (int)max_entries);

    // Copy to user buffer.
    fat32_entry_t* ubuf = (fat32_entry_t*)buf_ptr;
    for (int i = 0; i < count; i++) ubuf[i] = kbuf[i];

    kfree(kbuf);
    return (uint64_t)count;
}

// ── serial helper (debug only) ────────────────────────────────────────────
static void serial_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char* s) {
    for (; *s; s++) serial_putc(*s);
}

// ── syscall_dispatch ──────────────────────────────────────────────────────
uint64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4) {
    uint64_t ret;
    // For SYS_WRITE with fd=1, also echo to serial for visibility.
    if (nr == SYS_WRITE && arg1 == 1 && arg3 > 0) {
        const char* s = (const char*)arg2;
        for (uint64_t i = 0; i < arg3; i++) serial_putc(s[i]);
    }
    // Debug: log every syscall number.
    serial_putc('[');
    serial_putc('0' + (char)(nr % 10));
    serial_putc(']');
    switch (nr) {
        case SYS_WRITE:   ret = sys_write(arg1, arg2, arg3);      break;
        case SYS_EXIT:    ret = sys_exit(arg1);                    break;
        case SYS_READ:    ret = sys_read(arg1, arg2, arg3, arg4);  break;
        case SYS_OPEN:    ret = sys_open(arg1, arg2);              break;
        case SYS_CLOSE:   ret = sys_close(arg1);                   break;
        case SYS_BRK:     ret = sys_brk(arg1);                     break;
        case SYS_KILL:    ret = sys_kill(arg1, arg2);              break;
        case SYS_FORK:    ret = sys_fork();                         break;
        case SYS_EXEC:    ret = sys_exec(arg1, arg2);              break;
        case SYS_WAIT:    ret = sys_wait(arg1);                    break;
        case SYS_GETPID:  ret = sys_getpid();                      break;
        case SYS_READDIR: ret = sys_readdir(arg1, arg2);           break;
        case SYS_SPAWN:   ret = sys_spawn(arg1, arg2);             break;
        case SYS_THREAD:   ret = sys_thread(arg1, arg2, arg3);     break;
        case SYS_CLOCK_NS: ret = tsc_read_ns();                    break;
        default:           ret = (uint64_t)-1;                     break;
    }
    // Deliver any signals that were queued during or before this syscall.
    signal_deliver_pending();
    return ret;
}
