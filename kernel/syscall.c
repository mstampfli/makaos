#include "syscall.h"
#include "errno.h"
#include "common.h"
#include "sched.h"
#include "signal.h"
#include "process.h"
#include "mm.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "vfs.h"
#include "ext2.h"
#include "elf.h"
#include "tsc.h"

// g_vga is defined in idt.c — also used by vfs.c.
extern volatile uint16_t* g_vga;

// Scratch storage for the user register state saved at syscall entry.
uint64_t g_syscall_user_rsp    = 0;
uint64_t g_syscall_user_rip    = 0;
uint64_t g_syscall_user_rflags = 0;
uint64_t g_syscall_user_rbp    = 0;
uint64_t g_syscall_user_rbx    = 0;
uint64_t g_syscall_user_r12    = 0;
uint64_t g_syscall_user_r13    = 0;
uint64_t g_syscall_user_r14    = 0;
uint64_t g_syscall_user_r15    = 0;

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
    if (!g_current || fd >= g_current->files_shared->fd_capacity)
        return (uint64_t)-EBADF;
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-EBADF;
    if (!f->write) return (uint64_t)-EBADF;
    int64_t r = vfs_write(f, (const void*)buf, len);
    return (r < 0) ? (uint64_t)-EIO : (uint64_t)r;
}

// ── sys_read ──────────────────────────────────────────────────────────────
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags) {
    (void)flags;
    if (!g_current || fd >= g_current->files_shared->fd_capacity)
        return (uint64_t)-EBADF;
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-EBADF;
    int64_t r = vfs_read(f, (void*)buf, len);
    return (r < 0) ? (uint64_t)-EIO : (uint64_t)r;
}

// ── sys_open ──────────────────────────────────────────────────────────────
// open(path_ptr, flags, mode) → fd or -errno
// path: null-terminated absolute path.
// flags: O_RDONLY(0), O_WRONLY(1), O_RDWR(2), O_CREAT(0x40),
//        O_EXCL(0x80), O_TRUNC(0x200), O_APPEND(0x400).
// mode: ignored (no permission model yet).
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x040
#define O_EXCL    0x080
#define O_TRUNC   0x200
#define O_APPEND  0x400
static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode) {
    (void)mode;
    if (!g_current || !path_ptr) return (uint64_t)-EINVAL;

    // Copy null-terminated path from user space.
    char path[256];
    const char* upath = (const char*)path_ptr;
    uint64_t i = 0;
    while (i < 255 && upath[i]) { path[i] = upath[i]; i++; }
    path[i] = '\0';
    if (i == 0) return (uint64_t)-EINVAL;

    vfs_file_t* f = NULL;

    // /dev/ dispatch — map device names to built-in vfs_file_t objects.
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/') {
        const char* dev = path + 5;
        uint8_t match_kbd = (dev[0]=='k' && dev[1]=='b' && dev[2]=='d' && dev[3]=='\0');
        uint8_t match_vga = (dev[0]=='v' && dev[1]=='g' && dev[2]=='a' && dev[3]=='\0');
        if      (match_kbd) f = vfs_kbd_open();
        else if (match_vga) f = vfs_vga_open();
        else return (uint64_t)-ENOENT;
    } else {
        f = ext2_open(path);
        if (f && (flags & O_CREAT) && (flags & O_EXCL)) {
            // O_CREAT|O_EXCL: file must not already exist.
            vfs_close(f);
            return (uint64_t)-EEXIST;
        }
        if (!f && (flags & O_CREAT)) {
            // Create empty file then open it.
            if (!ext2_create(path)) return (uint64_t)-EIO;
            f = ext2_open(path);
        }
    }

    if (!f) return (uint64_t)-ENOENT;

    // Enforce access mode: strip write for O_RDONLY.
    if ((flags & 3) == O_RDONLY) f->write = NULL;

    // Propagate O_APPEND into vfs_file_t.flags for the write callback.
    if (flags & O_APPEND) f->flags |= O_APPEND;

    // O_TRUNC: truncate to zero on open (write modes only).
    if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR)))
        ext2_truncate(path);

    // Find the lowest free fd (skip 0/1/2 which are stdin/stdout/stderr).
    for (uint32_t fd = 3; ; fd++) {
        if (fd >= g_current->files_shared->fd_capacity) {
            if (!fd_table_grow(g_current->files_shared)) {
                vfs_close(f);
                return (uint64_t)-ENFILE;
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
    if (!g_current) return (uint64_t)-EBADF;
    if (fd >= g_current->files_shared->fd_capacity) return (uint64_t)-EBADF;
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-EBADF;
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
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;

    mm_t* mm = g_current->mm_shared->mm;

    // Query current brk.
    if (new_brk_raw == 0) return mm->brk;

    virt_addr_t new_brk = new_brk_raw; // exact value; demand-paging handles granularity

    // Reject below heap start.
    if (new_brk < mm->brk_start) return (uint64_t)-EINVAL;

    // Reject if new_brk would overlap with the stack region.
    // (Stack starts at VMM_USER_STACK_TOP - VMM_USER_STACK_PAGES * PAGE_SIZE)
    virt_addr_t stack_floor = VMM_USER_STACK_TOP - VMM_USER_STACK_PAGES * PAGE_SIZE;
    if (new_brk >= stack_floor) return (uint64_t)-ENOMEM;

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
        // Add enlarged VMA — end must be page-aligned for mm_vma_add, but
        // mm->brk stores the exact (possibly unaligned) value.
        virt_addr_t vma_end = (new_brk + PAGE_MASK) & ~PAGE_MASK;
        if (!mm_vma_add(mm, heap_vma_start, vma_end, VMA_R | VMA_W | VMA_ANON))
            return (uint64_t)-ENOMEM;

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
    if (g_current) {
        g_current->exit_code = (int32_t)(int)code;
        g_current->state = TASK_ZOMBIE;
        sched_add_zombie(g_current);
    }
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
    if (sig < 1 || sig >= NSIG) return (uint64_t)-EINVAL;

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
        if (!target) return (uint64_t)-ESRCH;
        signal_send(target, sig);
        return 0;
    }

    if (pid == 0) {
        if (!g_current) return (uint64_t)-ESRCH;
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
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;
    task_t* child = task_fork(g_current,
                              g_syscall_user_rip,
                              g_syscall_user_rflags,
                              g_syscall_user_rsp,
                              g_syscall_user_rbp,
                              g_syscall_user_rbx,
                              g_syscall_user_r12,
                              g_syscall_user_r13,
                              g_syscall_user_r14,
                              g_syscall_user_r15);
    if (!child) return (uint64_t)-ENOMEM;
    sched_add(child);
    return (uint64_t)child->pid;
}

// ── sys_exec ──────────────────────────────────────────────────────────────
// exec(path_ptr, pathlen) — replaces current address space with ELF at path.
static uint64_t sys_exec(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;
    if (pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;

    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    phys_addr_t new_pml4;
    mm_t*       new_mm;
    uint64_t    entry;

    vfs_file_t* f = ext2_open(path);
    if (!f) return (uint64_t)-ENOENT;

    const uint64_t MAX_ELF = 1024ULL * 1024ULL;
    uint8_t* buf = kmalloc(MAX_ELF);
    if (!buf) { vfs_close(f); return (uint64_t)-ENOMEM; }

    int64_t n = vfs_read(f, buf, MAX_ELF);
    vfs_close(f);

    if (n <= 0) { kfree(buf); return (uint64_t)-EIO; }

    uint8_t ok = elf_load_into(buf, (uint64_t)n, &new_pml4, &new_mm, &entry);
    kfree(buf);
    if (!ok) return (uint64_t)-ENOEXEC;

    // Swap in the new address space.
    vmm_free_user(g_current->mm_shared->pml4_phys);
    pmm_buddy_free(g_current->mm_shared->pml4_phys, 0);
    mm_destroy(g_current->mm_shared->mm);

    g_current->mm_shared->pml4_phys = new_pml4;
    g_current->mm_shared->mm        = new_mm;

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
    if (!g_current) return (uint64_t)-EINVAL;
    if (pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;

    // Copy path from user space.
    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    uint32_t pid = pid_alloc();
    if (!pid) return (uint64_t)-ENOMEM;

    task_t* child = elf_load_from_ext2(path, pid);
    if (!child) {
        pid_free(pid);
        return (uint64_t)-ENOENT;
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
    if (!g_current) return (uint64_t)-EINVAL;

    task_t* t = kmalloc(sizeof(task_t));
    if (!t) return (uint64_t)-ENOMEM;

    t->pid = pid_alloc();
    if (!t->pid) { kfree(t); return (uint64_t)-ENOMEM; }

    // Address space: share or deep-copy.
    if (flags & THREAD_SHARE_MM) {
        t->mm_shared = g_current->mm_shared;
        t->mm_shared->refs++;
    } else {
        phys_addr_t new_pml4 = vmm_alloc_pml4();
        if (new_pml4 == PMM_INVALID_ADDR) { pid_free(t->pid); kfree(t); return (uint64_t)-ENOMEM; }
        if (!vmm_clone_user(new_pml4, g_current->mm_shared->pml4_phys)) {
            vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0);
            pid_free(t->pid); kfree(t); return (uint64_t)-ENOMEM;
        }
        mm_t* new_mm = g_current->mm_shared->mm ? mm_clone(g_current->mm_shared->mm) : NULL;
        task_mm_t* tmm = task_mm_alloc(new_pml4, new_mm);
        if (!tmm) {
            if (new_mm) mm_destroy(new_mm);
            vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0);
            pid_free(t->pid); kfree(t); return (uint64_t)-ENOMEM;
        }
        t->mm_shared = tmm;
    }

    // File descriptors: share or copy.
    if (flags & THREAD_SHARE_FILES) {
        t->files_shared = g_current->files_shared;
        t->files_shared->refs++;
    } else {
        task_files_t* files = task_files_alloc();
        if (!files) { task_mm_release(t->mm_shared); pid_free(t->pid); kfree(t); return (uint64_t)-ENOMEM; }
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
// waitpid(pid, *status, options):
//   pid == -1: wait for any child.
//   options & WNOHANG: return 0 immediately if no child has exited.
// Returns child pid, 0 (WNOHANG + not ready), or -errno.
static uint64_t sys_wait(uint64_t pid_arg, uint64_t status_ptr, uint64_t options) {
    // pid == -1 (any child) maps to internal pid=0.
    int64_t signed_pid = (int64_t)pid_arg;
    uint32_t pid = (signed_pid == -1) ? 0 : (uint32_t)pid_arg;

    if (options & WNOHANG) {
        if (!sched_poll_pid(pid)) return 0; // not ready
    } else {
        sched_wait_pid(pid);
    }

    task_t* z = sched_reap_zombie(pid);
    if (z) {
        int32_t code = z->exit_code;
        uint32_t child_pid = z->pid;
        process_destroy(z);
        if (status_ptr) {
            int* sp = (int*)(uintptr_t)status_ptr;
            *sp = (int)((code & 0xFF) << 8);
        }
        return (uint64_t)child_pid;
    }
    // Child gone (killed by signal, no zombie).
    if (status_ptr) { int* sp = (int*)(uintptr_t)status_ptr; *sp = 0; }
    return (signed_pid == -1) ? (uint64_t)-ECHILD : pid_arg;
}

// ── sys_getppid ───────────────────────────────────────────────────────────
static uint64_t sys_getppid(void) {
    return g_current ? (uint64_t)g_current->ppid : 0;
}

// ── sys_getpid ────────────────────────────────────────────────────────────
static uint64_t sys_getpid(void) {
    return g_current ? (uint64_t)g_current->pid : 0;
}

// ── sys_readdir ───────────────────────────────────────────────────────────
// readdir(path_ptr, pathlen, buf_ptr, max) → entry count or -1
// buf_ptr points to an array of ext2_entry_t in user space.
static uint64_t sys_readdir(uint64_t path_ptr, uint64_t pathlen,
                             uint64_t buf_ptr,  uint64_t max_entries) {
    if (!buf_ptr || max_entries == 0) return 0;
    if (pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;
    if (max_entries > 256) max_entries = 256;

    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    ext2_entry_t* kbuf = kmalloc(max_entries * sizeof(ext2_entry_t));
    if (!kbuf) return (uint64_t)-ENOMEM;

    int count = ext2_readdir(path, kbuf, (int)max_entries);
    if (count < 0) { kfree(kbuf); return (uint64_t)-ENOENT; }

    ext2_entry_t* ubuf = (ext2_entry_t*)buf_ptr;
    for (int i = 0; i < count; i++) ubuf[i] = kbuf[i];

    kfree(kbuf);
    return (uint64_t)count;
}

// ── sys_stat ──────────────────────────────────────────────────────────────
// stat(path_ptr, pathlen, stat_ptr) → 0 or -1
// Finds the entry in the parent directory to get inode, size, and type.
static uint64_t sys_stat(uint64_t path_ptr, uint64_t pathlen, uint64_t stat_ptr) {
    if (!g_current || pathlen == 0 || pathlen > 255 || !stat_ptr) return (uint64_t)-EINVAL;

    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    // Special case: root directory.
    if (path[0] == '/' && path[1] == '\0') {
        stat_t* st = (stat_t*)stat_ptr;
        st->ino    = 2;
        st->size   = 0;
        st->mode   = 0x41ED;
        st->is_dir = 1;
        st->_pad   = 0;
        return 0;
    }

    // Split into parent + basename, then scan parent entries.
    // Find last '/' to get basename.
    uint64_t last = 0;
    for (uint64_t i = 0; i < pathlen; i++) if (path[i] == '/') last = i;
    const char* base = path + last + 1;
    char parent[256];
    if (last == 0) { parent[0] = '/'; parent[1] = '\0'; }
    else { for (uint64_t i = 0; i < last; i++) parent[i] = path[i]; parent[last] = '\0'; }

    ext2_entry_t* entries = kmalloc(256 * sizeof(ext2_entry_t));
    if (!entries) return (uint64_t)-ENOMEM;

    int count = ext2_readdir(parent, entries, 256);
    if (count < 0) { kfree(entries); return (uint64_t)-ENOENT; }

    stat_t* st = (stat_t*)stat_ptr;
    for (int i = 0; i < count; i++) {
        // Compare entry name with basename.
        const char* en = entries[i].name;
        const char* bn = base;
        while (*en && *en == *bn) { en++; bn++; }
        if (*en == '\0' && *bn == '\0') {
            st->ino    = entries[i].inode_num;
            st->size   = entries[i].size;
            st->mode   = entries[i].is_dir ? (uint16_t)0x41ED : (uint16_t)0x81A4;
            st->is_dir = entries[i].is_dir;
            st->_pad   = 0;
            kfree(entries);
            return 0;
        }
    }
    kfree(entries);
    return (uint64_t)-ENOENT;
}

// ── sys_unlink ────────────────────────────────────────────────────────────
// unlink(path_ptr, pathlen) → 0 or -1
static uint64_t sys_unlink(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current || pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;

    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    return ext2_unlink(path) ? 0 : (uint64_t)-ENOENT;
}

// ── sys_rename ────────────────────────────────────────────────────────────
// rename(src_ptr, srclen, dst_ptr, dstlen) → 0 or -1
static uint64_t sys_rename(uint64_t src_ptr, uint64_t srclen,
                            uint64_t dst_ptr, uint64_t dstlen) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (srclen == 0 || srclen > 255 || dstlen == 0 || dstlen > 255) return (uint64_t)-EINVAL;

    char src[256], dst[256];
    const char* usrc = (const char*)src_ptr;
    const char* udst = (const char*)dst_ptr;
    for (uint64_t i = 0; i < srclen; i++) src[i] = usrc[i];
    src[srclen] = '\0';
    for (uint64_t i = 0; i < dstlen; i++) dst[i] = udst[i];
    dst[dstlen] = '\0';

    return ext2_rename(src, dst) ? 0 : (uint64_t)-ENOENT;
}

// ── sys_getcwd ────────────────────────────────────────────────────────────
// getcwd(buf_ptr, buflen) → 0 or -1
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t buflen) {
    if (!g_current || !buf_ptr || buflen == 0) return (uint64_t)-EINVAL;

    char* ubuf = (char*)buf_ptr;
    const char* cwd = g_current->cwd;
    uint64_t i = 0;
    while (i + 1 < buflen && cwd[i]) { ubuf[i] = cwd[i]; i++; }
    ubuf[i] = '\0';
    return 0;
}

// ── sys_chdir ─────────────────────────────────────────────────────────────
// chdir(path_ptr, pathlen) → 0 or -1
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current || pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;

    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    // Verify it's a valid directory.
    ext2_entry_t tmp[1];
    if (ext2_readdir(path, tmp, 1) < 0) return (uint64_t)-ENOENT;

    for (uint32_t i = 0; i < 255 && path[i]; i++) g_current->cwd[i] = path[i];
    g_current->cwd[255] = '\0';
    // Ensure null terminator at the right place.
    uint32_t len = 0;
    while (len < 255 && path[len]) len++;
    g_current->cwd[len] = '\0';
    return 0;
}

// ── sys_mkdir ─────────────────────────────────────────────────────────────
// mkdir(path_ptr, pathlen) → 0 or -1
static uint64_t sys_mkdir(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current || pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;

    char path[256];
    const char* upath = (const char*)path_ptr;
    for (uint64_t i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';

    return ext2_mkdir(path) ? 0 : (uint64_t)-EIO;
}

// ── sys_dup ───────────────────────────────────────────────────────────────
// dup(oldfd) → new fd sharing the same file description, or -errno.
static uint64_t sys_dup(uint64_t oldfd) {
    if (!g_current || oldfd >= g_current->files_shared->fd_capacity)
        return (uint64_t)-EBADF;
    vfs_file_t* f = g_current->files_shared->fd_table[oldfd];
    if (!f) return (uint64_t)-EBADF;

    // Find lowest free fd.
    for (uint32_t fd = 0; ; fd++) {
        if (fd >= g_current->files_shared->fd_capacity) {
            if (!fd_table_grow(g_current->files_shared)) return (uint64_t)-ENFILE;
        }
        if (!g_current->files_shared->fd_table[fd]) {
            g_current->files_shared->fd_table[fd] = vfs_dup(f);
            return (uint64_t)fd;
        }
    }
}

// ── sys_dup2 ──────────────────────────────────────────────────────────────
// dup2(oldfd, newfd) → newfd sharing oldfd's file description, or -errno.
// If newfd == oldfd, returns newfd unchanged. Closes newfd if already open.
static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd) {
    if (!g_current) return (uint64_t)-EBADF;
    if (oldfd >= g_current->files_shared->fd_capacity) return (uint64_t)-EBADF;
    vfs_file_t* f = g_current->files_shared->fd_table[oldfd];
    if (!f) return (uint64_t)-EBADF;
    if (oldfd == newfd) return (uint64_t)newfd;

    // Grow fd table if needed.
    while (newfd >= g_current->files_shared->fd_capacity)
        if (!fd_table_grow(g_current->files_shared)) return (uint64_t)-ENFILE;

    // Close whatever is currently at newfd.
    vfs_file_t* old = g_current->files_shared->fd_table[newfd];
    if (old) vfs_close(old);

    g_current->files_shared->fd_table[newfd] = vfs_dup(f);
    return (uint64_t)newfd;
}

// ── sys_lseek ─────────────────────────────────────────────────────────────
// lseek(fd, offset, whence) → new file offset, or -errno
static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
    if (!g_current || fd >= g_current->files_shared->fd_capacity)
        return (uint64_t)-EBADF;
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-EBADF;
    if (!f->seek) return (uint64_t)-EINVAL;  // non-seekable (device, pipe)
    int64_t r = f->seek(f, (int64_t)offset, (int)(int64_t)whence);
    if (r < 0) return (uint64_t)-EINVAL;
    return (uint64_t)r;
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
        case SYS_OPEN:    ret = sys_open(arg1, arg2, arg3);         break;
        case SYS_CLOSE:   ret = sys_close(arg1);                   break;
        case SYS_BRK:     ret = sys_brk(arg1);                     break;
        case SYS_KILL:    ret = sys_kill(arg1, arg2);              break;
        case SYS_FORK:    ret = sys_fork();                         break;
        case SYS_EXEC:    ret = sys_exec(arg1, arg2);              break;
        case SYS_WAIT:    ret = sys_wait(arg1, arg2, arg3);         break;
        case SYS_GETPID:  ret = sys_getpid();                      break;
        case SYS_GETPPID: ret = sys_getppid();                     break;
        case SYS_READDIR: ret = sys_readdir(arg1, arg2, arg3, arg4); break;
        case SYS_SPAWN:   ret = sys_spawn(arg1, arg2);             break;
        case SYS_THREAD:   ret = sys_thread(arg1, arg2, arg3);     break;
        case SYS_CLOCK_NS: ret = tsc_read_ns();                    break;
        case SYS_STAT:    ret = sys_stat(arg1, arg2, arg3);        break;
        case SYS_UNLINK:  ret = sys_unlink(arg1, arg2);            break;
        case SYS_RENAME:  ret = sys_rename(arg1, arg2, arg3, arg4); break;
        case SYS_GETCWD:  ret = sys_getcwd(arg1, arg2);            break;
        case SYS_CHDIR:   ret = sys_chdir(arg1, arg2);             break;
        case SYS_MKDIR:   ret = sys_mkdir(arg1, arg2);             break;
        case SYS_LSEEK:   ret = sys_lseek(arg1, arg2, arg3);      break;
        case SYS_DUP:     ret = sys_dup(arg1);                    break;
        case SYS_DUP2:    ret = sys_dup2(arg1, arg2);             break;
        default:           ret = (uint64_t)-ENOSYS;               break;
    }
    // Deliver any signals that were queued during or before this syscall.
    signal_deliver_pending();
    return ret;
}
