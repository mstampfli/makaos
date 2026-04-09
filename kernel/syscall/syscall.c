#include "syscall.h"
#include "errno.h"
#include "pipe.h"
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
#include "fb.h"
#include "net/socket.h"
#include "net/net.h"

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

// Extra syscall arguments for 6-argument syscalls (mmap).
// Saved by syscall_entry.asm before calling syscall_dispatch.
uint64_t g_syscall_arg5 = 0;   // r8  (mmap fd)
uint64_t g_syscall_arg6 = 0;   // r9  (mmap offset)

// Exec redirect globals (read by syscall_entry.asm after exec syscall).
uint8_t     g_exec_requested = 0;
uint64_t    g_exec_entry     = 0;
uint64_t    g_exec_rsp       = 0;
phys_addr_t g_exec_pml4      = 0;

// Signal delivery globals (read by syscall_entry.asm before sysretq).
// g_signal_deliver: 1 = entering user handler, 2 = sigreturn restore.
uint8_t  g_signal_deliver    = 0;
uint64_t g_signal_rdi        = 0;  // signum passed to handler (rdi)
// Set to 1 while syscall_dispatch is calling signal_deliver_pending so that
// signal_deliver_pending knows it's safe to set up user-space signal frames.
uint8_t  g_signal_in_syscall = 0;

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
    if (r < 0) return (uint64_t)r; // propagate errno from driver (e.g. -EPIPE)
    return (uint64_t)r;
}

// ── user_buf_prefault ─────────────────────────────────────────────────────
// Demand-page every not-yet-mapped page in [addr, addr+len) for the current
// task.  This prevents kernel-mode page faults when vfs_read() writes directly
// into a user buffer that spans pages not yet backed by physical frames.
//
// Must be called while the process's PML4 is active in CR3 (i.e. inside a
// syscall handler, before any address-space switch).
static void user_buf_prefault(virt_addr_t addr, size_t len) {
    if (!len || !g_current) return;
    mm_t* mm = g_current->mm_shared ? g_current->mm_shared->mm : NULL;
    if (!mm) return;

    virt_addr_t page = addr & ~(virt_addr_t)0xFFF;
    virt_addr_t end  = (addr + len + 0xFFF) & ~(virt_addr_t)0xFFF;
    phys_addr_t pml4 = vmm_current_pml4();  // current CR3 = this process's PML4

    for (; page < end; page += 0x1000) {
        // Walk VMA list — if no VMA covers this page it's not a valid mapping.
        vma_t* vma = mm_vma_find(mm, page);
        if (!vma) continue;

        // Check if this page is already present.  vmm_page_unmap returns 0 and
        // does nothing if the PTE is absent, so we use it safely as a probe only
        // when we need to distinguish. Simpler: just try to map; if the PTE is
        // already present vmm_page_map re-writes it with the same flags (harmless).
        // But we must avoid allocating a frame when the page is already mapped.
        // Use a volatile read of the PTE via the HHDM — walk the 4-level table.
        {
            // PT walk: PML4[39:47] → PDPT[30:38] → PD[21:29] → PT[12:20].
            uint64_t* pml4v = (uint64_t*)(pml4 + HHDM_OFFSET);
            uint64_t  e4 = pml4v[(page >> 39) & 0x1FF];
            if (!(e4 & PAGE_PRESENT)) goto map_page;
            uint64_t* pdpt = (uint64_t*)((e4 & PAGE_ADDR_MASK) + HHDM_OFFSET);
            uint64_t  e3 = pdpt[(page >> 30) & 0x1FF];
            if (!(e3 & PAGE_PRESENT)) goto map_page;
            uint64_t* pd = (uint64_t*)((e3 & PAGE_ADDR_MASK) + HHDM_OFFSET);
            uint64_t  e2 = pd[(page >> 21) & 0x1FF];
            if (!(e2 & PAGE_PRESENT)) goto map_page;
            uint64_t* pt = (uint64_t*)((e2 & PAGE_ADDR_MASK) + HHDM_OFFSET);
            uint64_t  e1 = pt[(page >> 12) & 0x1FF];
            if (e1 & PAGE_PRESENT) continue;  // already mapped — skip
        }

    map_page:;
        // Allocate a frame, zero it, map it with the VMA's permissions.
        phys_addr_t frame = pmm_buddy_alloc(0);
        if (frame == PMM_INVALID_ADDR) break;  // OOM — abort prefault

        uint64_t* p = (uint64_t*)(frame + HHDM_OFFSET);
        for (int i = 0; i < 512; i++) p[i] = 0;

        if (!vmm_page_map(pml4, page, frame, mm_vma_pte_flags(vma->flags))) {
            pmm_buddy_free(frame, 0);
        }
    }
}

// ── sys_read ──────────────────────────────────────────────────────────────
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags) {
    (void)flags;
    if (!g_current || fd >= g_current->files_shared->fd_capacity)
        return (uint64_t)-EBADF;
    vfs_file_t* f = g_current->files_shared->fd_table[fd];
    if (!f) return (uint64_t)-EBADF;
    // Pre-fault the user buffer so kernel writes into it don't kernel-panic.
    user_buf_prefault((virt_addr_t)buf, (size_t)len);
    int64_t r = vfs_read(f, (void*)buf, len);
    return (r < 0) ? (uint64_t)-EIO : (uint64_t)r;
}

// ── sys_open ──────────────────────────────────────────────────────────────
// open(path_ptr, flags, mode) → fd or -errno
// flags: O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_EXCL/O_TRUNC/O_APPEND/O_NONBLOCK/O_CLOEXEC
// mode: used for umask masking on O_CREAT.
static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode) {
    if (!g_current || !path_ptr) return (uint64_t)-EINVAL;

    // Copy null-terminated path from user space (max 511 to leave room for cwd prefix).
    char raw[512];
    const char* upath = (const char*)path_ptr;
    uint64_t i = 0;
    while (i < 511 && upath[i]) { raw[i] = upath[i]; i++; }
    raw[i] = '\0';
    if (i == 0) return (uint64_t)-EINVAL;

    // Resolve relative paths: prepend cwd if path does not start with '/'.
    char path[512];
    if (raw[0] != '/') {
        const char* cwd = g_current->cwd;
        uint64_t clen = 0;
        while (cwd[clen]) clen++;
        uint64_t j = 0;
        for (; j < clen && j < 510; j++) path[j] = cwd[j];
        // Ensure cwd ends with '/'.
        if (j > 0 && path[j-1] != '/') path[j++] = '/';
        for (uint64_t k = 0; raw[k] && j < 511; k++, j++) path[j] = raw[k];
        path[j] = '\0';
    } else {
        for (uint64_t k = 0; k <= i; k++) path[k] = raw[k];
    }

    vfs_file_t* f = NULL;

    // /dev/ dispatch — map device names to built-in vfs_file_t objects.
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/') {
        const char* dev = path + 5;
        uint8_t match_kbd    = (dev[0]=='k' && dev[1]=='b' && dev[2]=='d' && dev[3]=='\0');
        uint8_t match_kbdraw = (dev[0]=='k' && dev[1]=='b' && dev[2]=='d' && dev[3]=='r'
                             && dev[4]=='a' && dev[5]=='w' && dev[6]=='\0');
        uint8_t match_vga    = (dev[0]=='v' && dev[1]=='g' && dev[2]=='a' && dev[3]=='\0');
        uint8_t match_mouse  = (dev[0]=='m' && dev[1]=='o' && dev[2]=='u' && dev[3]=='s'
                             && dev[4]=='e' && dev[5]=='\0');
        uint8_t match_dsp    = (dev[0]=='d' && dev[1]=='s' && dev[2]=='p' && dev[3]=='\0');
        if      (match_kbdraw) f = vfs_kbdraw_open();
        else if (match_kbd)    f = vfs_kbd_open();
        else if (match_vga)    f = vfs_vga_open();
        else if (match_mouse)  f = vfs_mouse_open();
        else if (match_dsp)    f = vfs_dsp_open();
        else return (uint64_t)-ENOENT;
    } else {
        f = ext2_open(path);
        if (f && (flags & O_CREAT) && (flags & O_EXCL)) {
            // O_CREAT|O_EXCL: file must not already exist.
            vfs_close(f);
            return (uint64_t)-EEXIST;
        }
        if (!f && (flags & O_CREAT)) {
            // Apply umask to mode before creating.
            mode &= ~(uint64_t)g_current->umask;
            if (!ext2_create(path)) return (uint64_t)-EIO;
            f = ext2_open(path);
        }
    }

    if (!f) return (uint64_t)-ENOENT;

    // Enforce access mode: strip write for O_RDONLY.
    if ((flags & 3) == O_RDONLY) f->write = NULL;

    // Propagate O_APPEND/O_NONBLOCK into vfs_file_t.flags.
    if (flags & O_APPEND)    f->flags |= O_APPEND;
    if (flags & O_NONBLOCK)  f->flags |= O_NONBLOCK;

    // O_TRUNC: truncate to zero on open (write modes only).
    if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR)))
        ext2_truncate(path);

    // Find the lowest free fd.
    for (uint32_t fd = 0; ; fd++) {
        if (fd >= g_current->files_shared->fd_capacity) {
            if (!fd_table_grow(g_current->files_shared)) {
                vfs_close(f);
                return (uint64_t)-ENFILE;
            }
        }
        if (!g_current->files_shared->fd_table[fd]) {
            g_current->files_shared->fd_table[fd] = f;
            // O_CLOEXEC: set FD_CLOEXEC atomically.
            g_current->files_shared->fd_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
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
    g_current->files_shared->fd_flags[fd] = 0;
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

// ── sys_kill helpers (file-scope so clang accepts them) ───────────────────
typedef struct { int64_t pid; task_t* result; } find_arg_t;

static void kill_find_cb(task_t* t, void* data) {
    find_arg_t* a = (find_arg_t*)data;
    if (!a->result && (int64_t)t->pid == a->pid) a->result = t;
}

static int s_bcast_sig = 0;
static void kill_bcast_cb(task_t* t, void* data) {
    (void)data;
    if (!(t->flags & TASK_FLAG_KTHREAD)) signal_send(t, s_bcast_sig);
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
        task_t* target = NULL;
        if (g_current && (int64_t)g_current->pid == pid)
            target = g_current;
        if (!target) {
            find_arg_t a = {pid, NULL};
            sched_for_each(kill_find_cb, &a);
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
    s_bcast_sig = sig;
    sched_for_each(kill_bcast_cb, NULL);
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

// ── sys_exec (execve) ─────────────────────────────────────────────────────
// execve(path_ptr, argv_ptr, envp_ptr)
//
// path_ptr — user pointer to NUL-terminated path string
// argv_ptr — user pointer to NULL-terminated array of user char* pointers
// envp_ptr — user pointer to NULL-terminated array of user char* pointers
//
// Replaces the current address space with the ELF at path.
// Closes all fds with FD_CLOEXEC set.
// Inherits pgid, sid, cwd, umask.
// On success does not return (triggers sysretq to new entry point via
// g_exec_requested mechanism).
static uint64_t sys_exec(uint64_t path_ptr, uint64_t argv_ptr, uint64_t envp_ptr) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;
    if (!path_ptr) return (uint64_t)-EINVAL;

    // ── Copy path from user space ─────────────────────────────────────────
    char path[512];
    const char* upath = (const char*)path_ptr;
    uint64_t plen = 0;
    while (plen < 511 && upath[plen]) { path[plen] = upath[plen]; plen++; }
    path[plen] = '\0';
    if (plen == 0) return (uint64_t)-EINVAL;

    // Resolve relative path using cwd.
    char resolved[512];
    if (path[0] != '/') {
        const char* cwd = g_current->cwd;
        uint64_t clen = 0; while (cwd[clen]) clen++;
        uint64_t j = 0;
        for (; j < clen && j < 510; j++) resolved[j] = cwd[j];
        if (j > 0 && resolved[j-1] != '/') resolved[j++] = '/';
        for (uint64_t k = 0; path[k] && j < 511; k++, j++) resolved[j] = path[k];
        resolved[j] = '\0';
    } else {
        for (uint64_t k = 0; k <= plen; k++) resolved[k] = path[k];
    }

    // ── Copy argv from user space (up to 256 args, 4KiB each) ────────────
    #define MAX_ARGS  256
    #define MAX_ARG_LEN 4096
    char* k_argv[MAX_ARGS + 1];
    uint32_t argc = 0;
    if (argv_ptr) {
        const uint64_t* uargv = (const uint64_t*)argv_ptr;
        while (argc < MAX_ARGS) {
            uint64_t ustr = uargv[argc];
            if (!ustr) break;
            const char* s = (const char*)ustr;
            uint64_t l = 0; while (l < MAX_ARG_LEN && s[l]) l++;
            char* ks = kmalloc(l + 1);
            if (!ks) goto oom;
            for (uint64_t i = 0; i < l; i++) ks[i] = s[i];
            ks[l] = '\0';
            k_argv[argc++] = ks;
        }
    }
    k_argv[argc] = NULL;

    // ── Copy envp from user space (up to 512 vars) ────────────────────────
    #define MAX_ENVS 512
    char* k_envp[MAX_ENVS + 1];
    uint32_t envc = 0;
    if (envp_ptr) {
        const uint64_t* uenvp = (const uint64_t*)envp_ptr;
        while (envc < MAX_ENVS) {
            uint64_t ustr = uenvp[envc];
            if (!ustr) break;
            const char* s = (const char*)ustr;
            uint64_t l = 0; while (l < MAX_ARG_LEN && s[l]) l++;
            char* ks = kmalloc(l + 1);
            if (!ks) goto oom;
            for (uint64_t i = 0; i < l; i++) ks[i] = s[i];
            ks[l] = '\0';
            k_envp[envc++] = ks;
        }
    }
    k_envp[envc] = NULL;

    // ── Load ELF ──────────────────────────────────────────────────────────
    vfs_file_t* f = ext2_open(resolved);
    if (!f) { goto enoent; }

    const uint64_t MAX_ELF = 16ULL * 1024ULL * 1024ULL; // 16 MiB
    uint8_t* buf = kmalloc(MAX_ELF);
    if (!buf) { vfs_close(f); goto oom; }

    int64_t n = vfs_read(f, buf, MAX_ELF);
    vfs_close(f);
    if (n <= 0) { kfree(buf); goto enoent; }

    phys_addr_t new_pml4;
    mm_t*       new_mm;
    uint64_t    entry;
    uint64_t    phdr_vaddr = 0;
    uint16_t    phnum = 0, phent = 0;

    if (!elf_load_into(buf, (uint64_t)n, &new_pml4, &new_mm, &entry,
                       &phdr_vaddr, &phnum, &phent)) {
        kfree(buf);
        goto enoexec;
    }
    kfree(buf);

    // ── Build user stack with argv/envp/auxv ──────────────────────────────
    uint64_t user_rsp = elf_setup_stack(new_pml4,
                                         (const char* const*)k_argv,
                                         (const char* const*)k_envp,
                                         entry, phdr_vaddr, phnum, phent);
    if (!user_rsp) {
        vmm_free_user(new_pml4); pmm_buddy_free(new_pml4, 0); mm_destroy(new_mm);
        goto oom;
    }

    // ── Free kernel copies of argv/envp ───────────────────────────────────
    for (uint32_t i = 0; i < argc; i++) kfree(k_argv[i]);
    for (uint32_t i = 0; i < envc; i++) kfree(k_envp[i]);

    // ── Close FD_CLOEXEC fds ──────────────────────────────────────────────
    {
        task_files_t* files = g_current->files_shared;
        for (uint32_t i = 0; i < files->fd_capacity; i++) {
            if (files->fd_table[i] && (files->fd_flags[i] & FD_CLOEXEC)) {
                vfs_close(files->fd_table[i]);
                files->fd_table[i] = NULL;
                files->fd_flags[i] = 0;
            }
        }
    }

    // ── Swap in new address space ─────────────────────────────────────────
    vmm_free_user(g_current->mm_shared->pml4_phys);
    pmm_buddy_free(g_current->mm_shared->pml4_phys, 0);
    mm_destroy(g_current->mm_shared->mm);

    g_current->mm_shared->pml4_phys = new_pml4;
    g_current->mm_shared->mm        = new_mm;

    // Reset signal handlers to SIG_DFL (POSIX: exec clears custom handlers).
    for (int si = 0; si < NSIG; si++) {
        if (g_current->sigstate.handlers[si].sa_handler > SIG_IGN)
            g_current->sigstate.handlers[si].sa_handler = SIG_DFL;
        g_current->sigstate.handlers[si].sa_mask  = 0;
        g_current->sigstate.handlers[si].sa_flags = 0;
    }
    // Unblock all signals (POSIX: exec resets the signal mask).
    g_current->sigstate.blocked = 0;

    // ── Trigger sysretq to new entry point ────────────────────────────────
    g_exec_entry     = entry;
    g_exec_rsp       = user_rsp;
    g_exec_pml4      = new_pml4;
    g_exec_requested = 1;
    return 0;

oom:
    for (uint32_t i = 0; i < argc; i++) kfree(k_argv[i]);
    for (uint32_t i = 0; i < envc; i++) kfree(k_envp[i]);
    return (uint64_t)-ENOMEM;
enoent:
    for (uint32_t i = 0; i < argc; i++) kfree(k_argv[i]);
    for (uint32_t i = 0; i < envc; i++) kfree(k_envp[i]);
    return (uint64_t)-ENOENT;
enoexec:
    for (uint32_t i = 0; i < argc; i++) kfree(k_argv[i]);
    for (uint32_t i = 0; i < envc; i++) kfree(k_envp[i]);
    return (uint64_t)-ENOEXEC;
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

    const char* argv[] = { path, NULL };
    const char* envp[] = { "PATH=/bin", "HOME=/", "TERM=linux", NULL };
    task_t* child = elf_exec_from_ext2(path, pid, argv, envp);
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

// ── sys_pipe ──────────────────────────────────────────────────────────────
// pipe(fds_ptr): fills fds[0]=read, fds[1]=write. Returns 0 or -errno.
static uint64_t sys_pipe(uint64_t fds_ptr) {
    if (!g_current || !fds_ptr) return (uint64_t)-EINVAL;
    int* fds = (int*)(uintptr_t)fds_ptr;

    vfs_file_t* r;
    vfs_file_t* w;
    int err = pipe_create(&r, &w);
    if (err) return (uint64_t)(int64_t)err;

    // Allocate two fds.
    int rfd = -1, wfd = -1;
    for (uint32_t fd = 0; ; fd++) {
        if (fd >= g_current->files_shared->fd_capacity)
            if (!fd_table_grow(g_current->files_shared)) goto fail;
        if (!g_current->files_shared->fd_table[fd]) {
            if (rfd < 0) { rfd = (int)fd; g_current->files_shared->fd_table[fd] = r; }
            else         { wfd = (int)fd; g_current->files_shared->fd_table[fd] = w; break; }
        }
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
fail:
    vfs_close(r);
    vfs_close(w);
    return (uint64_t)-ENFILE;
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

// ── sys_sigaction ─────────────────────────────────────────────────────────
// sigaction(sig, act_ptr, oldact_ptr)
// act_ptr / oldact_ptr point to a user-space struct matching k_sigaction_t.
static uint64_t sys_sigaction(uint64_t sig, uint64_t act_ptr, uint64_t oldact_ptr) {
    if (sig < 1 || sig >= NSIG) return (uint64_t)-EINVAL;
    if (sig == SIGKILL || sig == SIGSEGV) return (uint64_t)-EINVAL;

    k_sigaction_t* ka = &g_current->sigstate.handlers[sig];

    if (oldact_ptr) {
        k_sigaction_t* old = (k_sigaction_t*)oldact_ptr;
        old->sa_handler  = ka->sa_handler;
        old->sa_restorer = ka->sa_restorer;
        old->sa_mask     = ka->sa_mask;
        old->sa_flags    = ka->sa_flags;
    }
    if (act_ptr) {
        k_sigaction_t* act = (k_sigaction_t*)act_ptr;
        ka->sa_handler  = act->sa_handler;
        ka->sa_restorer = act->sa_restorer;
        ka->sa_mask     = act->sa_mask;
        ka->sa_flags    = act->sa_flags;
    }
    return 0;
}

// ── sys_sigprocmask ───────────────────────────────────────────────────────
// sigprocmask(how, set_ptr, oldset_ptr)
// set_ptr / oldset_ptr point to uint32_t signal masks.
static uint64_t sys_sigprocmask(uint64_t how, uint64_t set_ptr, uint64_t oldset_ptr) {
    uint32_t* blocked = &g_current->sigstate.blocked;
    if (oldset_ptr) *(uint32_t*)oldset_ptr = *blocked;
    if (set_ptr) {
        uint32_t set = *(uint32_t*)set_ptr;
        set &= ~(1u << (SIGKILL - 1));  // SIGKILL can never be blocked
        if (how == SIG_BLOCK)        *blocked |= set;
        else if (how == SIG_UNBLOCK) *blocked &= ~set;
        else if (how == SIG_SETMASK) *blocked = set;
        else return (uint64_t)-EINVAL;
    }
    return 0;
}

// ── sys_sigreturn ─────────────────────────────────────────────────────────
// Called by the restorer trampoline after a signal handler returns.
// Restores the interrupted user context from the sigframe saved on the stack.
static uint64_t sys_sigreturn(void) {
    uint64_t frame_base = g_current->sigstate.sigframe_rsp;
    if (!frame_base) return (uint64_t)-EINVAL;

    sigframe_t* frame = (sigframe_t*)frame_base;

    // Restore user register context via the globals that syscall_entry.asm reads.
    g_syscall_user_rip    = frame->rip;
    g_syscall_user_rsp    = frame->rsp;
    g_syscall_user_rflags = frame->rflags;
    g_syscall_user_rbp    = frame->rbp;
    g_syscall_user_rbx    = frame->rbx;
    g_syscall_user_r12    = frame->r12;
    g_syscall_user_r13    = frame->r13;
    g_syscall_user_r14    = frame->r14;
    g_syscall_user_r15    = frame->r15;

    // Restore the signal mask that was active before the handler ran.
    g_current->sigstate.blocked = frame->blocked;
    g_current->sigstate.sigframe_rsp = 0;

    // Ask syscall_entry.asm to override rcx/r11/rsp/rbp/rbx/r12-r15 before sysretq.
    g_signal_deliver = 2;
    return 0;
}

// ── sys_mmap ──────────────────────────────────────────────────────────────
// mmap(addr, len, prot, flags, fd, off) → mapped address or -errno.
// Only anonymous private mappings are supported (MAP_ANONYMOUS | MAP_PRIVATE).
// fd must be -1 for anonymous mappings.  MAP_FIXED is supported.
static uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t off) {
    (void)off;
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;
    if (!len) return (uint64_t)-EINVAL;

    // Only anonymous mappings for now.
    if (!(flags & MAP_ANONYMOUS)) return (uint64_t)-ENOSYS;
    if ((int64_t)fd != -1)        return (uint64_t)-ENOSYS;

    mm_t* mm = g_current->mm_shared->mm;

    // Round len up to page boundary.
    len = (len + PAGE_MASK) & ~PAGE_MASK;

    virt_addr_t vaddr;
    if (flags & MAP_FIXED) {
        if (!addr || (addr & PAGE_MASK)) return (uint64_t)-EINVAL;
        // Unmap anything in the range first (POSIX requirement).
        phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
        for (virt_addr_t p = addr; p < addr + len; p += PAGE_SIZE) {
            phys_addr_t frame;
            if (vmm_page_unmap(pml4, p, &frame))
                pmm_buddy_free(frame, 0);
        }
        mm_vma_remove(mm, addr, addr + len);
        vaddr = addr;
    } else if (addr) {
        // Use addr as a hint: if not free, find a free region anyway.
        addr &= ~PAGE_MASK;
        // Check if the hint region is free.
        uint8_t free_hint = 1;
        for (vma_t* v = mm->vmas; v; v = v->next) {
            if (addr < v->end && addr + len > v->start) { free_hint = 0; break; }
        }
        vaddr = free_hint ? addr : mm_vma_find_free(mm, len);
    } else {
        vaddr = mm_vma_find_free(mm, len);
    }

    if (!vaddr) return (uint64_t)-ENOMEM;

    // Convert PROT_* to VMA flags.
    uint32_t vma_flags = VMA_ANON;
    if (prot & PROT_READ)  vma_flags |= VMA_R;
    if (prot & PROT_WRITE) vma_flags |= VMA_W;
    if (prot & PROT_EXEC)  vma_flags |= VMA_X;
    // At minimum allow read so the page-fault handler can map the page.
    if (!vma_flags) vma_flags = VMA_ANON; // PROT_NONE: still need VMA entry

    if (!mm_vma_add(mm, vaddr, vaddr + len, vma_flags))
        return (uint64_t)-ENOMEM;

    // Pages are demand-paged — do not allocate physical frames here.
    return vaddr;
}

// ── sys_munmap ────────────────────────────────────────────────────────────
static uint64_t sys_munmap(uint64_t addr, uint64_t len) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;
    if (!addr || (addr & PAGE_MASK))              return (uint64_t)-EINVAL;
    if (!len)                                     return (uint64_t)-EINVAL;

    len = (len + PAGE_MASK) & ~PAGE_MASK;
    phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
    mm_t* mm = g_current->mm_shared->mm;

    // Unmap physical pages and free frames.
    for (virt_addr_t p = addr; p < addr + len; p += PAGE_SIZE) {
        phys_addr_t frame;
        if (vmm_page_unmap(pml4, p, &frame))
            pmm_buddy_free(frame, 0);
    }

    // Remove VMA descriptors covering the range.
    mm_vma_remove(mm, addr, addr + len);
    return 0;
}

// ── sys_nanosleep ─────────────────────────────────────────────────────────
// nanosleep(req, rem) — sleep for at least req->tv_sec * 1e9 + req->tv_nsec ns.
// rem is ignored (no partial-sleep resume after signal for now).
typedef struct { uint64_t tv_sec; uint64_t tv_nsec; } k_timespec_t;

static uint64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr) {
    (void)rem_ptr;
    if (!req_ptr) return (uint64_t)-EINVAL;
    k_timespec_t* req = (k_timespec_t*)req_ptr;
    if (req->tv_nsec >= 1000000000ULL) return (uint64_t)-EINVAL;

    uint64_t sleep_ns = req->tv_sec * 1000000000ULL + req->tv_nsec;
    if (!sleep_ns) return 0;

    uint64_t wake_ns = tsc_read_ns() + sleep_ns;

    // Sleep until deadline: add to sleep list so timer tick can wake us.
    while (tsc_read_ns() < wake_ns) {
        g_current->sleep_until_ns = wake_ns;
        sched_sleep();  // sets TASK_SLEEPING, adds to s_sleep_head, yields
        g_current->sleep_until_ns = 0;
    }
    return 0;
}

// ── sys_gettimeofday ──────────────────────────────────────────────────────
// gettimeofday(tv, tz) — fills struct timeval { tv_sec, tv_usec }.
typedef struct { int64_t tv_sec; int64_t tv_usec; } k_timeval_t;

static uint64_t sys_gettimeofday(uint64_t tv_ptr, uint64_t tz_ptr) {
    (void)tz_ptr;
    if (!tv_ptr) return (uint64_t)-EINVAL;
    uint64_t ns = tsc_read_ns();
    k_timeval_t* tv = (k_timeval_t*)tv_ptr;
    tv->tv_sec  = (int64_t)(ns / 1000000000ULL);
    tv->tv_usec = (int64_t)((ns % 1000000000ULL) / 1000ULL);
    return 0;
}

// ── sys_fb_blit ───────────────────────────────────────────────────────────
// fb_blit(src_rgba, src_w, src_h, flags)
// Scales src_w × src_h RGBA (0xRRGGBBAA) buffer to fill the screen.
// Uses nearest-neighbour scaling.  src is a user-space pointer.
static uint64_t sys_fb_blit(uint64_t src_ptr, uint64_t src_w,
                             uint64_t src_h, uint64_t flags) {
    (void)flags;
    if (!src_ptr || !src_w || !src_h) return (uint64_t)-EINVAL;
    if (!g_fb.base_virt)              return (uint64_t)-EIO;

    user_buf_prefault(src_ptr, src_w * src_h * 4);
    const uint32_t* src   = (const uint32_t*)src_ptr;
    uint32_t*       fb    = (uint32_t*)g_fb.base_virt;
    uint32_t        dw    = g_fb.width;
    uint32_t        dh    = g_fb.height;
    uint32_t        pitch = g_fb.pitch / 4; // pitch in pixels

    // Nearest-neighbour scale: for each destination pixel find source pixel.
    // Use fixed-point 16.16 arithmetic for speed.
    uint32_t step_x = (uint32_t)((src_w  << 16) / dw);
    uint32_t step_y = (uint32_t)((src_h  << 16) / dh);
    uint32_t sy = 0;
    for (uint32_t y = 0; y < dh; y++, sy += step_y) {
        uint32_t src_row = (sy >> 16) * (uint32_t)src_w;
        uint32_t sx = 0;
        for (uint32_t x = 0; x < dw; x++, sx += step_x) {
            // Source pixel: 0xRRGGBBAA (R in highest byte).
            uint32_t rgba = src[src_row + (sx >> 16)];
            uint8_t r = (uint8_t)(rgba >> 24);
            uint8_t g = (uint8_t)(rgba >> 16);
            uint8_t b = (uint8_t)(rgba >>  8);
            // BGRX: bytes B,G,R,X in memory = uint32 B|(G<<8)|(R<<16).
            fb[y * pitch + x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
        }
    }
    return 0;
}

// ── sys_fb_info ───────────────────────────────────────────────────────────
// fb_info(user_fb_info_t*) — fills user struct with framebuffer parameters.
typedef struct {
    uint64_t phys;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} user_fb_info_t;

static uint64_t sys_fb_info(uint64_t ptr) {
    if (!ptr) return (uint64_t)-EINVAL;
    if (!g_fb.base_virt) return (uint64_t)-EIO;
    user_fb_info_t* info = (user_fb_info_t*)ptr;
    info->phys   = g_fb.base_virt - HHDM_OFFSET;
    info->width  = g_fb.width;
    info->height = g_fb.height;
    info->pitch  = g_fb.pitch;
    info->bpp    = g_fb.bpp;
    return 0;
}

// ── Socket syscalls ───────────────────────────────────────────────────────

// Helper: allocate the lowest available fd ≥ 3 and assign f to it.
static int64_t fd_install(vfs_file_t* f) {
    if (!g_current) return -EBADF;
    for (uint32_t fd = 3; ; fd++) {
        if (fd >= g_current->files_shared->fd_capacity) {
            if (!fd_table_grow(g_current->files_shared)) {
                vfs_close(f);
                return -ENFILE;
            }
        }
        if (!g_current->files_shared->fd_table[fd]) {
            g_current->files_shared->fd_table[fd] = f;
            return (int64_t)fd;
        }
    }
}

static vfs_file_t* fd_to_file(uint64_t fd) {
    if (!g_current) return NULL;
    if (fd >= g_current->files_shared->fd_capacity) return NULL;
    return g_current->files_shared->fd_table[fd];
}

// socket(domain, type, proto) → fd or -errno
static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto) {
    (void)proto;
    if (!net_ready()) return (uint64_t)-ENETDOWN;
    vfs_file_t* f = socket_open((int)domain, (int)type);
    if (!f) return (uint64_t)-ENOMEM;
    int64_t fd = fd_install(f);
    return (fd < 0) ? (uint64_t)fd : (uint64_t)fd;
}

// bind(fd, sockaddr_in*, addrlen) → 0 or -errno
static uint64_t sys_bind(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen) {
    (void)addrlen;
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    const sockaddr_in_t* sa = (const sockaddr_in_t*)addr_ptr;
    if (!sa) return (uint64_t)-EINVAL;
    // Port is in network byte order in sockaddr_in; convert to host order.
    uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));
    int r = socket_bind(f, port);
    return (r == 0) ? 0 : (uint64_t)-EINVAL;
}

// listen(fd, backlog) → 0 or -errno
static uint64_t sys_listen(uint64_t fd, uint64_t backlog) {
    (void)backlog;
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    int r = socket_listen(f);
    return (r == 0) ? 0 : (uint64_t)-EINVAL;
}

// accept(fd, sockaddr_in*, addrlen*) → new fd or -errno
static uint64_t sys_accept(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    (void)addrlen_ptr;
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    sockaddr_in_t* peer = (sockaddr_in_t*)addr_ptr;  // may be NULL
    vfs_file_t* cf = socket_accept(f, peer);
    if (!cf) return (uint64_t)-ECONNABORTED;
    int64_t nfd = fd_install(cf);
    return (nfd < 0) ? (uint64_t)nfd : (uint64_t)nfd;
}

// connect(fd, sockaddr_in*, addrlen) → 0 or -errno
static uint64_t sys_connect(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen) {
    (void)addrlen;
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    const sockaddr_in_t* sa = (const sockaddr_in_t*)addr_ptr;
    if (!sa) return (uint64_t)-EINVAL;
    uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));
    int r = socket_connect(f, sa->sin_addr, port);
    return (r == 0) ? 0 : (uint64_t)-ECONNREFUSED;
}

// sendto(fd, buf, len, flags, addr, addrlen) → bytes or -errno
// For TCP sockets addr may be NULL.
static uint64_t sys_sendto(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                             uint64_t flags, uint64_t addr_ptr, uint64_t addrlen) {
    (void)flags; (void)addrlen;
    vfs_file_t* f = fd_to_file(fd);
    if (!f || !buf_ptr || !len) return (uint64_t)-EINVAL;
    const sockaddr_in_t* sa = (const sockaddr_in_t*)addr_ptr;
    int r;
    if (sa) {
        uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));
        r = socket_sendto(f, (const void*)buf_ptr, (uint32_t)len,
                           sa->sin_addr, port);
    } else {
        r = socket_send(f, (const void*)buf_ptr, (uint32_t)len);
    }
    return (r < 0) ? (uint64_t)-EIO : (uint64_t)r;
}

// recvfrom(fd, buf, len, flags, addr, addrlen*) → bytes or -errno
static uint64_t sys_recvfrom(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                               uint64_t flags, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    (void)flags; (void)addrlen_ptr;
    vfs_file_t* f = fd_to_file(fd);
    if (!f || !buf_ptr || !len) return (uint64_t)-EINVAL;
    sockaddr_in_t* sa = (sockaddr_in_t*)addr_ptr;  // may be NULL
    int r;
    if (sa) {
        r = socket_recvfrom(f, (void*)buf_ptr, (uint32_t)len, sa);
        // Convert port back to network byte order in the returned addr.
        if (r >= 0 && sa)
            sa->sin_port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));
    } else {
        r = socket_recv(f, (void*)buf_ptr, (uint32_t)len);
    }
    return (r < 0) ? (uint64_t)-EIO : (uint64_t)r;
}

// setsockopt — stub (returns 0 for anything we don't understand)
static uint64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t opt,
                                uint64_t val_ptr, uint64_t vallen) {
    (void)fd; (void)level; (void)opt; (void)val_ptr; (void)vallen;
    return 0;
}

// shutdown(fd, how) → 0 or -errno
static uint64_t sys_shutdown(uint64_t fd, uint64_t how) {
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    int r = socket_shutdown(f, (int)how);
    return (r == 0) ? 0 : (uint64_t)-EINVAL;
}

// ── Global terminal state (shared across all processes using the tty) ─────
// In a real kernel this lives in a struct tty; here we keep one global
// terminal termios and window size. Both are readable/writable by any
// process via TCGETS/TCSETS/TIOCGWINSZ/TIOCSWINSZ.

static termios_t g_tty_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = B38400 | CS8 | CREAD | HUPCL | CLOCAL,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
    .c_line  = 0,
    .c_cc    = {
        [VINTR]    = 3,    // ^C
        [VQUIT]    = 28,   // backslash
        [VERASE]   = 127,  // DEL
        [VKILL]    = 21,   // ^U
        [VEOF]     = 4,    // ^D
        [VTIME]    = 0,
        [VMIN]     = 1,
        [VSWTC]    = 0,
        [VSTART]   = 17,   // ^Q
        [VSTOP]    = 19,   // ^S
        [VSUSP]    = 26,   // ^Z
        [VEOL]     = 0,
        [VREPRINT] = 18,   // ^R
        [VDISCARD] = 15,   // ^O
        [VWERASE]  = 23,   // ^W
        [VLNEXT]   = 22,   // ^V
        [VEOL2]    = 0,
    },
};

static winsize_t g_tty_winsize = {
    .ws_row    = 25,
    .ws_col    = 80,
    .ws_xpixel = 0,
    .ws_ypixel = 0,
};

// Foreground process group of the controlling terminal.
static uint32_t g_tty_fg_pgid = 1;

// ── Helper: copy bytes from user to kernel safely ─────────────────────────
// Returns 0 on success, -EFAULT if the pointer is obviously bad.
static int copy_from_user(void* dst, const void* src_u, uint64_t len) {
    if (!src_u) return -EFAULT;
    user_buf_prefault((virt_addr_t)src_u, len);
    const uint8_t* s = (const uint8_t*)src_u;
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

static int copy_to_user(void* dst_u, const void* src, uint64_t len) {
    if (!dst_u) return -EFAULT;
    user_buf_prefault((virt_addr_t)dst_u, len);
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst_u;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

// ── sys_fcntl ─────────────────────────────────────────────────────────────
// fcntl(fd, cmd, arg) → varies
static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    if (!g_current) return (uint64_t)-EBADF;
    task_files_t* files = g_current->files_shared;
    if (fd >= files->fd_capacity || !files->fd_table[fd])
        return (uint64_t)-EBADF;

    switch ((int)cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        // Dup to lowest fd >= arg.
        uint32_t start = (uint32_t)arg;
        for (uint32_t nfd = start; ; nfd++) {
            if (nfd >= files->fd_capacity) {
                if (!fd_table_grow(files)) return (uint64_t)-EMFILE;
            }
            if (!files->fd_table[nfd]) {
                vfs_file_t* orig = files->fd_table[fd];
                vfs_dup(orig);
                files->fd_table[nfd] = orig;
                files->fd_flags[nfd] = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
                return (uint64_t)nfd;
            }
        }
    }
    case F_GETFD:
        return (uint64_t)files->fd_flags[fd];
    case F_SETFD:
        files->fd_flags[fd] = (uint32_t)(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL: {
        vfs_file_t* f = files->fd_table[fd];
        uint64_t fl = 0;
        if (f->write && f->read) fl = O_RDWR;
        else if (f->write)       fl = O_WRONLY;
        else                     fl = O_RDONLY;
        if (f->flags & O_APPEND)   fl |= O_APPEND;
        if (f->flags & O_NONBLOCK) fl |= O_NONBLOCK;
        return fl;
    }
    case F_SETFL: {
        vfs_file_t* f = files->fd_table[fd];
        // Only O_APPEND and O_NONBLOCK are changeable after open.
        if (arg & O_APPEND)   f->flags |= O_APPEND;
        else                  f->flags &= ~(uint32_t)O_APPEND;
        if (arg & O_NONBLOCK) f->flags |= O_NONBLOCK;
        else                  f->flags &= ~(uint32_t)O_NONBLOCK;
        return 0;
    }
    default:
        return (uint64_t)-EINVAL;
    }
}

// ── sys_fstat ─────────────────────────────────────────────────────────────
// fstat(fd, stat_t*) → 0 or -errno
static uint64_t sys_fstat(uint64_t fd, uint64_t stat_ptr) {
    if (!g_current || !stat_ptr) return (uint64_t)-EINVAL;
    task_files_t* files = g_current->files_shared;
    if (fd >= files->fd_capacity || !files->fd_table[fd])
        return (uint64_t)-EBADF;

    vfs_file_t* f = files->fd_table[fd];
    stat_t st = {0};

    // If the file has a stat path stored, use ext2.
    if (f->path[0]) {
        // Try ext2 stat.
        ext2_inode_t inode;
        uint32_t inum = ext2_lookup_path(f->path);
        if (inum && ext2_read_inode(inum, &inode)) {
            st.ino    = inum;
            st.size   = inode.i_size;
            st.mode   = inode.i_mode;
            st.is_dir = ((inode.i_mode & 0xF000) == 0x4000) ? 1 : 0;
        }
    } else {
        // Device/pipe/socket: fill in a minimal stat.
        st.mode   = 0666;
        st.is_dir = 0;
    }

    return (copy_to_user((void*)stat_ptr, &st, sizeof(st)) == 0) ? 0 : (uint64_t)-EFAULT;
}

// ── sys_access ────────────────────────────────────────────────────────────
// access(path, mode) → 0 or -errno
// We don't have a real permission model: if the file exists it's accessible.
static uint64_t sys_access(uint64_t path_ptr, uint64_t amode) {
    if (!g_current || !path_ptr) return (uint64_t)-EINVAL;
    (void)amode;

    char raw[512];
    const char* upath = (const char*)path_ptr;
    uint64_t i = 0;
    while (i < 511 && upath[i]) { raw[i] = upath[i]; i++; }
    raw[i] = '\0';

    char path[512];
    if (raw[0] != '/') {
        const char* cwd = g_current->cwd;
        uint64_t clen = 0; while (cwd[clen]) clen++;
        uint64_t j = 0;
        for (; j < clen && j < 510; j++) path[j] = cwd[j];
        if (j > 0 && path[j-1] != '/') path[j++] = '/';
        for (uint64_t k = 0; raw[k] && j < 511; k++, j++) path[j] = raw[k];
        path[j] = '\0';
    } else {
        for (uint64_t k = 0; k <= i; k++) path[k] = raw[k];
    }

    // /dev/* always exists.
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/')
        return 0;

    vfs_file_t* f = ext2_open(path);
    if (!f) return (uint64_t)-ENOENT;
    vfs_close(f);
    return 0;
}

// ── sys_uname ─────────────────────────────────────────────────────────────
// uname(utsname*) → 0 or -errno
static void strncpy_k(char* dst, const char* src, uint64_t n) {
    uint64_t i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static uint64_t sys_uname(uint64_t buf_ptr) {
    if (!buf_ptr) return (uint64_t)-EINVAL;
    utsname_t u;
    strncpy_k(u.sysname,    "MakaOS",          65);
    strncpy_k(u.nodename,   "makaos",          65);
    strncpy_k(u.release,    "0.1.0",           65);
    strncpy_k(u.version,    "#1 SMP",          65);
    strncpy_k(u.machine,    "x86_64",          65);
    strncpy_k(u.domainname, "(none)",          65);
    return (copy_to_user((void*)buf_ptr, &u, sizeof(u)) == 0) ? 0 : (uint64_t)-EFAULT;
}

// ── sys_umask ─────────────────────────────────────────────────────────────
// umask(mask) → old mask
static uint64_t sys_umask(uint64_t mask) {
    if (!g_current) return (uint64_t)-EINVAL;
    uint32_t old = g_current->umask;
    g_current->umask = (uint16_t)(mask & 0777);
    return (uint64_t)old;
}

// ── Identity syscalls (single-user OS: always uid=0, root) ───────────────
static uint64_t sys_getuid(void)   { return 0; }
static uint64_t sys_geteuid(void)  { return 0; }
static uint64_t sys_getgid(void)   { return 0; }
static uint64_t sys_getegid(void)  { return 0; }
static uint64_t sys_getgroups(uint64_t size, uint64_t list_ptr) {
    // We have one supplementary group: gid 0.
    if ((int64_t)size < 0) return (uint64_t)-EINVAL;
    if (size == 0) return 1; // just return count
    uint32_t gid = 0;
    if (copy_to_user((void*)list_ptr, &gid, sizeof(gid)) != 0)
        return (uint64_t)-EFAULT;
    return 1;
}

// ── Process group / session syscalls ─────────────────────────────────────

// Find a task by pid. Returns NULL if not found.
// We scan the scheduler's run queue. This is O(n) but correct.
extern task_t* sched_find_pid(uint32_t pid);

static uint64_t sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg) {
    uint32_t pid  = pid_arg  ? (uint32_t)pid_arg  : g_current->pid;
    uint32_t pgid = pgid_arg ? (uint32_t)pgid_arg : pid;

    task_t* t = (pid == g_current->pid) ? g_current : sched_find_pid(pid);
    if (!t) return (uint64_t)-ESRCH;

    // Can't change pgid of a session leader.
    if (t->pid == t->sid) return (uint64_t)-EPERM;

    t->pgid = pgid;
    return 0;
}

static uint64_t sys_getpgid(uint64_t pid_arg) {
    if (pid_arg == 0) return (uint64_t)g_current->pgid;
    task_t* t = sched_find_pid((uint32_t)pid_arg);
    if (!t) return (uint64_t)-ESRCH;
    return (uint64_t)t->pgid;
}

static uint64_t sys_getpgrp(void) {
    return (uint64_t)g_current->pgid;
}

static uint64_t sys_setsid(void) {
    if (!g_current) return (uint64_t)-EINVAL;
    // If we're already a process group leader we can't create a new session.
    if (g_current->pid == g_current->pgid) return (uint64_t)-EPERM;
    g_current->sid  = g_current->pid;
    g_current->pgid = g_current->pid;
    // New session has no controlling terminal — set tty fg pgid to us.
    g_tty_fg_pgid = g_current->pid;
    return (uint64_t)g_current->sid;
}

static uint64_t sys_getsid(uint64_t pid_arg) {
    if (pid_arg == 0) return (uint64_t)g_current->sid;
    task_t* t = sched_find_pid((uint32_t)pid_arg);
    if (!t) return (uint64_t)-ESRCH;
    return (uint64_t)t->sid;
}

// ── tcgetpgrp / tcsetpgrp ─────────────────────────────────────────────────
static uint64_t sys_tcgetpgrp(uint64_t fd) {
    (void)fd; // we have one global tty
    return (uint64_t)g_tty_fg_pgid;
}

static uint64_t sys_tcsetpgrp(uint64_t fd, uint64_t pgid) {
    (void)fd;
    g_tty_fg_pgid = (uint32_t)pgid;
    return 0;
}

// ── sys_ioctl ─────────────────────────────────────────────────────────────
// ioctl(fd, request, arg) → varies
static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
    if (!g_current) return (uint64_t)-EBADF;

    // Validate fd (allow 0/1/2 which are tty fds).
    if (fd < g_current->files_shared->fd_capacity &&
        !g_current->files_shared->fd_table[fd] && fd > 2)
        return (uint64_t)-EBADF;

    switch (request) {
    case TIOCGWINSZ:
        return (copy_to_user((void*)arg, &g_tty_winsize, sizeof(g_tty_winsize)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TIOCSWINSZ:
        return (copy_from_user(&g_tty_winsize, (const void*)arg, sizeof(g_tty_winsize)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TIOCGPGRP:
        return (copy_to_user((void*)arg, &g_tty_fg_pgid, sizeof(g_tty_fg_pgid)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TIOCSPGRP: {
        uint32_t pg = 0;
        if (copy_from_user(&pg, (const void*)arg, sizeof(pg)) != 0)
            return (uint64_t)-EFAULT;
        g_tty_fg_pgid = pg;
        return 0;
    }
    case TCGETS:
        return (copy_to_user((void*)arg, &g_tty_termios, sizeof(g_tty_termios)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return (copy_from_user(&g_tty_termios, (const void*)arg, sizeof(g_tty_termios)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TCSBRK:
    case TCXONC:
    case TCFLSH:
    case TIOCEXCL:
    case TIOCNXCL:
        return 0;  // Acknowledged, no-op.
    case TIOCSCTTY:
        // Claim controlling terminal for this session.
        g_tty_fg_pgid = g_current->pgid;
        return 0;
    case TIOCGSERIAL:
        // Return all-zeros serial_struct — bash checks this on some paths.
        if (arg) {
            uint8_t* p = (uint8_t*)arg;
            for (int i = 0; i < 60; i++) p[i] = 0; // sizeof(serial_struct) ≈ 60
        }
        return 0;
    default:
        return (uint64_t)-EINVAL;
    }
}

// ── sys_select ────────────────────────────────────────────────────────────
// select(nfds, readfds*, writefds*, exceptfds*, timeval*) → count or -errno
// Simplified: we consider a fd readable if it has data waiting, writable
// if it's not full, and we do not sleep — we just poll once.
// A NULL fd_set means "don't care about this set".
// timeval = {0,0} → non-blocking; NULL → block until at least one is ready.
typedef struct { int64_t tv_sec; int64_t tv_usec; } timeval_t;

static int fd_is_readable(vfs_file_t* f) {
    if (!f || !f->read) return 0;
    // Pipes: check if data is available without blocking.
    if (f->poll) return f->poll(f, POLLIN);
    return 1; // regular files and devices are always ready
}

static int fd_is_writable(vfs_file_t* f) {
    if (!f || !f->write) return 0;
    if (f->poll) return f->poll(f, POLLOUT);
    return 1;
}

static uint64_t sys_select(uint64_t nfds, uint64_t rset_ptr, uint64_t wset_ptr,
                            uint64_t eset_ptr, uint64_t tv_ptr) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (nfds > FD_SETSIZE) return (uint64_t)-EINVAL;

    fd_set_t rset, wset, eset;
    fd_set_t rout, wout, eout;
    for (int i = 0; i < (int)(FD_SETSIZE/64); i++) {
        rset.bits[i] = rset_ptr ? ((fd_set_t*)rset_ptr)->bits[i] : 0;
        wset.bits[i] = wset_ptr ? ((fd_set_t*)wset_ptr)->bits[i] : 0;
        eset.bits[i] = eset_ptr ? ((fd_set_t*)eset_ptr)->bits[i] : 0;
        rout.bits[i] = wout.bits[i] = eout.bits[i] = 0;
    }

    // Determine timeout in nanoseconds (NULL = infinite).
    uint64_t timeout_ns = UINT64_MAX;
    if (tv_ptr) {
        timeval_t tv;
        copy_from_user(&tv, (const void*)tv_ptr, sizeof(tv));
        timeout_ns = (uint64_t)tv.tv_sec * 1000000000ULL
                   + (uint64_t)tv.tv_usec * 1000ULL;
    }

    extern uint64_t tsc_read_ns(void);
    uint64_t deadline = tsc_read_ns() + timeout_ns;
    int count = 0;

    do {
        count = 0;
        task_files_t* files = g_current->files_shared;
        for (uint32_t fd = 0; fd < nfds; fd++) {
            vfs_file_t* f = (fd < files->fd_capacity) ? files->fd_table[fd] : NULL;
            if (FD_ISSET(fd, &rset) && fd_is_readable(f)) {
                rout.bits[fd/64] |= (1ULL << (fd%64)); count++;
            }
            if (FD_ISSET(fd, &wset) && fd_is_writable(f)) {
                wout.bits[fd/64] |= (1ULL << (fd%64)); count++;
            }
            if (FD_ISSET(fd, &eset)) {
                // No exceptional conditions in our model.
            }
        }
        if (count > 0) break;
        if (timeout_ns == 0) break;
        // Yield and retry until timeout.
        extern void sched_yield(void);
        sched_yield();
    } while (tsc_read_ns() < deadline);

    // Write back output sets.
    if (rset_ptr) copy_to_user((void*)rset_ptr, &rout, sizeof(rout));
    if (wset_ptr) copy_to_user((void*)wset_ptr, &wout, sizeof(wout));
    if (eset_ptr) copy_to_user((void*)eset_ptr, &eout, sizeof(eout));

    return (uint64_t)(int64_t)count;
}

// ── sys_poll ──────────────────────────────────────────────────────────────
// poll(pollfd_t*, nfds, timeout_ms) → count or -errno
static uint64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms) {
    if (!g_current || !fds_ptr) return (uint64_t)-EINVAL;
    if (nfds > 1024) return (uint64_t)-EINVAL;

    pollfd_t* ufds = (pollfd_t*)fds_ptr;
    task_files_t* files = g_current->files_shared;

    uint64_t timeout_ns = (timeout_ms == (uint64_t)-1) ? UINT64_MAX
                        : (timeout_ms * 1000000ULL);
    extern uint64_t tsc_read_ns(void);
    uint64_t deadline = tsc_read_ns() + timeout_ns;
    int count = 0;

    do {
        count = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            ufds[i].revents = 0;
            int fd = ufds[i].fd;
            if (fd < 0) continue;
            vfs_file_t* f = ((uint32_t)fd < files->fd_capacity) ? files->fd_table[fd] : NULL;
            if (!f) { ufds[i].revents = POLLNVAL; count++; continue; }

            uint16_t rev = 0;
            if ((ufds[i].events & POLLIN)  && fd_is_readable(f))  rev |= POLLIN;
            if ((ufds[i].events & POLLOUT) && fd_is_writable(f)) rev |= POLLOUT;
            if (rev) { ufds[i].revents = rev; count++; }
        }
        if (count > 0) break;
        if (timeout_ms == 0) break;
        extern void sched_yield(void);
        sched_yield();
    } while (tsc_read_ns() < deadline);

    return (uint64_t)(int64_t)count;
}

// ── sys_readlink ──────────────────────────────────────────────────────────
// readlink(path, buf, bufsz) → len or -errno
// We have no real symlink support in ext2 yet; return EINVAL for everything
// except /proc/self/exe which many programs query.
static uint64_t sys_readlink(uint64_t path_ptr, uint64_t buf_ptr, uint64_t bufsz) {
    if (!g_current || !path_ptr || !buf_ptr) return (uint64_t)-EINVAL;

    const char* upath = (const char*)path_ptr;
    // Match /proc/self/exe → return the last exec'd path (stored in cwd area).
    // For now return a plausible path.
    const char* target = "/bin/bash";
    uint64_t tlen = 0; while (target[tlen]) tlen++;
    if (tlen > bufsz) tlen = bufsz;
    copy_to_user((void*)buf_ptr, target, tlen);
    (void)upath;
    return (uint64_t)tlen;
}

// ── sys_symlink / sys_link ────────────────────────────────────────────────
// symlink and link are not fully supported; return EPERM for now so bash
// degrades gracefully rather than crashing.
static uint64_t sys_symlink(uint64_t target_ptr, uint64_t link_ptr) {
    (void)target_ptr; (void)link_ptr;
    return (uint64_t)-EPERM;
}

static uint64_t sys_link(uint64_t old_ptr, uint64_t new_ptr) {
    (void)old_ptr; (void)new_ptr;
    return (uint64_t)-EPERM;
}

// ── sys_chmod / sys_fchmod ────────────────────────────────────────────────
// No permission enforcement; accept the call and return success.
static uint64_t sys_chmod(uint64_t path_ptr, uint64_t mode) {
    (void)path_ptr; (void)mode;
    return 0;
}
static uint64_t sys_fchmod(uint64_t fd, uint64_t mode) {
    (void)fd; (void)mode;
    return 0;
}

// ── sys_chown / sys_fchown ────────────────────────────────────────────────
static uint64_t sys_chown(uint64_t path_ptr, uint64_t uid, uint64_t gid) {
    (void)path_ptr; (void)uid; (void)gid;
    return 0;
}
static uint64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid) {
    (void)fd; (void)uid; (void)gid;
    return 0;
}

// ── sys_truncate / sys_ftruncate ──────────────────────────────────────────
// truncate(path, length) → 0 or -errno
static uint64_t sys_truncate(uint64_t path_ptr, uint64_t length) {
    if (!g_current || !path_ptr) return (uint64_t)-EINVAL;
    char raw[512];
    const char* upath = (const char*)path_ptr;
    uint64_t i = 0;
    while (i < 511 && upath[i]) { raw[i] = upath[i]; i++; }
    raw[i] = '\0';
    char path[512];
    if (raw[0] != '/') {
        const char* cwd = g_current->cwd;
        uint64_t clen = 0; while (cwd[clen]) clen++;
        uint64_t j = 0;
        for (; j < clen && j < 510; j++) path[j] = cwd[j];
        if (j > 0 && path[j-1] != '/') path[j++] = '/';
        for (uint64_t k = 0; raw[k] && j < 511; k++, j++) path[j] = raw[k];
        path[j] = '\0';
    } else {
        for (uint64_t k = 0; k <= i; k++) path[k] = raw[k];
    }
    if (!ext2_truncate_to(path, length)) return (uint64_t)-EIO;
    return 0;
}

// ftruncate(fd, length) → 0 or -errno
static uint64_t sys_ftruncate(uint64_t fd, uint64_t length) {
    if (!g_current) return (uint64_t)-EBADF;
    task_files_t* files = g_current->files_shared;
    if (fd >= files->fd_capacity || !files->fd_table[fd])
        return (uint64_t)-EBADF;
    vfs_file_t* f = files->fd_table[fd];
    if (!f->path[0]) return (uint64_t)-EINVAL; // pipes/devices can't be truncated
    if (!ext2_truncate_to(f->path, length)) return (uint64_t)-EIO;
    return 0;
}

// ── sys_times ─────────────────────────────────────────────────────────────
// times(tms*) → clock ticks since boot (or -errno)
// Clock tick = 100 Hz (CLKTCK).
static uint64_t sys_times(uint64_t buf_ptr) {
    extern uint64_t tsc_read_ns(void);
    uint64_t ns = tsc_read_ns();
    uint64_t ticks = ns / 10000000ULL; // 100 Hz → 10 ms per tick

    if (buf_ptr) {
        tms_t tms = { ticks, 0, 0, 0 };
        copy_to_user((void*)buf_ptr, &tms, sizeof(tms));
    }
    return ticks;
}

// ── sys_getrusage ─────────────────────────────────────────────────────────
// getrusage(who, rusage*) → 0 or -errno
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
static uint64_t sys_getrusage(uint64_t who, uint64_t buf_ptr) {
    (void)who;
    if (!buf_ptr) return (uint64_t)-EINVAL;
    rusage_t ru = {0};
    return (copy_to_user((void*)buf_ptr, &ru, sizeof(ru)) == 0) ? 0 : (uint64_t)-EFAULT;
}

// ── serial helper (debug only) ────────────────────────────────────────────
static void serial_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char* s) {
    for (; *s; s++) serial_putc(*s);
}

// ── native_syscall_dispatch ───────────────────────────────────────────────
// Dispatches using our internal syscall numbers.
uint64_t native_syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2,
                                  uint64_t arg3, uint64_t arg4) {
    uint64_t ret;
    // Mirror stdout/stderr writes to serial for debugging.
    if (nr == SYS_WRITE && (arg1 == 1 || arg1 == 2) && arg3 > 0) {
        const char* s = (const char*)arg2;
        for (uint64_t i = 0; i < arg3; i++) serial_putc(s[i]);
    }
    switch (nr) {
        case SYS_WRITE:   ret = sys_write(arg1, arg2, arg3);      break;
        case SYS_EXIT:    ret = sys_exit(arg1);                    break;
        case SYS_READ:    ret = sys_read(arg1, arg2, arg3, arg4);  break;
        case SYS_OPEN:    ret = sys_open(arg1, arg2, arg3);         break;
        case SYS_CLOSE:   ret = sys_close(arg1);                   break;
        case SYS_BRK:     ret = sys_brk(arg1);                     break;
        case SYS_KILL:    ret = sys_kill(arg1, arg2);              break;
        case SYS_FORK:    ret = sys_fork();                         break;
        case SYS_EXEC:    ret = sys_exec(arg1, arg2, arg3);         break;
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
        case SYS_DUP:          ret = sys_dup(arg1);                       break;
        case SYS_DUP2:         ret = sys_dup2(arg1, arg2);                break;
        case SYS_PIPE:         ret = sys_pipe(arg1);                      break;
        case SYS_SIGACTION:    ret = sys_sigaction(arg1, arg2, arg3);          break;
        case SYS_SIGPROCMASK:  ret = sys_sigprocmask(arg1, arg2, arg3);        break;
        case SYS_SIGRETURN:    ret = sys_sigreturn();                           break;
        case SYS_MMAP:         ret = sys_mmap(arg1, arg2, arg3, arg4,
                                              g_syscall_arg5, g_syscall_arg6);  break;
        case SYS_MUNMAP:       ret = sys_munmap(arg1, arg2);                    break;
        case SYS_NANOSLEEP:    ret = sys_nanosleep(arg1, arg2);                 break;
        case SYS_GETTOD:       ret = sys_gettimeofday(arg1, arg2);              break;
        case SYS_FB_BLIT:      ret = sys_fb_blit(arg1, arg2, arg3, arg4);      break;
        case SYS_FB_INFO:      ret = sys_fb_info(arg1);                         break;
        case SYS_SOCKET:       ret = sys_socket(arg1, arg2, arg3);              break;
        case SYS_BIND:         ret = sys_bind(arg1, arg2, arg3);                break;
        case SYS_LISTEN:       ret = sys_listen(arg1, arg2);                    break;
        case SYS_ACCEPT:       ret = sys_accept(arg1, arg2, arg3);              break;
        case SYS_CONNECT:      ret = sys_connect(arg1, arg2, arg3);             break;
        case SYS_SENDTO:       ret = sys_sendto(arg1, arg2, arg3, arg4,
                                                g_syscall_arg5, g_syscall_arg6); break;
        case SYS_RECVFROM:     ret = sys_recvfrom(arg1, arg2, arg3, arg4,
                                                   g_syscall_arg5,
                                                   g_syscall_arg6);              break;
        case SYS_SETSOCKOPT:   ret = sys_setsockopt(arg1, arg2, arg3, arg4,
                                                     g_syscall_arg5);            break;
        case SYS_SHUTDOWN:     ret = sys_shutdown(arg1, arg2);                  break;

        // ── POSIX bash-compat syscalls ────────────────────────────────────
        case SYS_FCNTL:      ret = sys_fcntl(arg1, arg2, arg3);               break;
        case SYS_FSTAT:      ret = sys_fstat(arg1, arg2);                     break;
        case SYS_ACCESS:     ret = sys_access(arg1, arg2);                    break;
        case SYS_UNAME:      ret = sys_uname(arg1);                           break;
        case SYS_UMASK:      ret = sys_umask(arg1);                           break;
        case SYS_GETUID:     ret = sys_getuid();                              break;
        case SYS_GETEUID:    ret = sys_geteuid();                             break;
        case SYS_GETGID:     ret = sys_getgid();                              break;
        case SYS_GETEGID:    ret = sys_getegid();                             break;
        case SYS_GETGROUPS:  ret = sys_getgroups(arg1, arg2);                break;
        case SYS_SETPGID:    ret = sys_setpgid(arg1, arg2);                  break;
        case SYS_GETPGID:    ret = sys_getpgid(arg1);                        break;
        case SYS_GETPGRP:    ret = sys_getpgrp();                             break;
        case SYS_SETSID:     ret = sys_setsid();                              break;
        case SYS_GETSID:     ret = sys_getsid(arg1);                         break;
        case SYS_IOCTL:      ret = sys_ioctl(arg1, arg2, arg3);              break;
        case SYS_SELECT:     ret = sys_select(arg1, arg2, arg3, arg4,
                                              g_syscall_arg5);                break;
        case SYS_POLL:       ret = sys_poll(arg1, arg2, arg3);               break;
        case SYS_READLINK:   ret = sys_readlink(arg1, arg2, arg3);           break;
        case SYS_SYMLINK:    ret = sys_symlink(arg1, arg2);                  break;
        case SYS_LINK:       ret = sys_link(arg1, arg2);                     break;
        case SYS_CHMOD:      ret = sys_chmod(arg1, arg2);                    break;
        case SYS_FCHMOD:     ret = sys_fchmod(arg1, arg2);                   break;
        case SYS_CHOWN:      ret = sys_chown(arg1, arg2, arg3);              break;
        case SYS_FCHOWN:     ret = sys_fchown(arg1, arg2, arg3);             break;
        case SYS_TRUNCATE:   ret = sys_truncate(arg1, arg2);                 break;
        case SYS_FTRUNCATE:  ret = sys_ftruncate(arg1, arg2);                break;
        case SYS_TIMES:      ret = sys_times(arg1);                          break;
        case SYS_GETRUSAGE:  ret = sys_getrusage(arg1, arg2);                break;
        case SYS_TCGETPGRP:  ret = sys_tcgetpgrp(arg1);                      break;
        case SYS_TCSETPGRP:  ret = sys_tcsetpgrp(arg1, arg2);                break;
        case SYS_REBOOT:
            outb(0x64, 0xFE);
            for (;;) __asm__ volatile("cli; hlt");
            break;

        default:               ret = (uint64_t)-ENOSYS;                         break;
    }
    // Deliver any pending signals on the syscall return path.
    // g_signal_in_syscall=1 tells signal_deliver_pending it may set up user frames.
    g_signal_in_syscall = 1;
    signal_deliver_pending();
    g_signal_in_syscall = 0;
    return ret;
}

// ── syscall_dispatch ──────────────────────────────────────────────────────
// Entry point from syscall_entry.asm.
uint64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4) {
    return native_syscall_dispatch(nr, arg1, arg2, arg3, arg4);
}
