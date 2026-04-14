#include "syscall.h"
#include "errno.h"
#include "pipe.h"
#include "common.h"
#include "sched.h"
#include "signal.h"
#include "process.h"
#include "mm.h"
#include "shmem.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "vfs.h"
#include "ext2.h"
#include "elf.h"
#include "tsc.h"
#include "fb.h"
#include "tty.h"
#include "evdev.h"
#include "proc.h"
#include "net/socket.h"
#include "net/unix_sock.h"
#include "net/net.h"
#include "net/virtio_net.h"
#include "cred.h"
#include "rights.h"
#include "pledge.h"
#include "unveil.h"
#include "perm.h"
#include "ksec.h"
#include "virtfs.h"

// g_vga is defined in idt.c — also used by vfs.c.
extern volatile uint16_t* g_vga;

// Forward declarations for serial debug helpers (defined near bottom of file).
static void serial_putc(char c);
static void serial_puts(const char* s);
static void serial_hex_u32(uint32_t v);

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

// Forward declaration — defined near copy_from/to_user below.
static inline int _access_ok(uint64_t addr, uint64_t len);

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
    // Rights check: only enforce if rights field is non-zero (stamped at open).
    // Zero means the fd was opened before rights stamping existed (legacy path)
    // or is a device opened via tty_open/evdev_open — treat as fully permitted.
    if (f->rights != 0 && !rights_check(f->rights, RIGHT_WRITE))
        return (uint64_t)-EACCES;
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

        // Zero the full page (512 × 8 bytes = 4096).
        __builtin_memset((void*)(frame + HHDM_OFFSET), 0, PAGE_SIZE);

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
    // Rights check: zero means legacy/device fd — treat as permitted.
    if (f->rights != 0 && !rights_check(f->rights, RIGHT_READ))
        return (uint64_t)-EACCES;
    // Pre-fault the user buffer so kernel writes into it don't kernel-panic.
    user_buf_prefault((virt_addr_t)buf, (size_t)len);
    int64_t r = vfs_read(f, (void*)buf, len);
    return (uint64_t)r;  // pass errno through (r<0 is -errno already)
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

    // ── Permission check + existence test via unified fs_lookup ──────────
    // Covers both virtfs (/proc, /dev) and ext2 in one call.
    // fsn.inode_nr is the already-resolved ext2 inode — no second lookup needed.
    uint8_t open_need = ACL_PERM_READ;
    {
        int oflags_acc = (int)(flags & 3);
        if (oflags_acc == O_WRONLY || oflags_acc == O_RDWR) open_need |= ACL_PERM_WRITE;
    }
    fs_node_t fsn;
    int fsr = fs_lookup(path, &g_current->cred, open_need, &fsn);
    if (fsr == 0 && (flags & O_CREAT) && (flags & O_EXCL))
        return (uint64_t)-EEXIST;
    if (fsr != 0 && fsr != -ENOENT) return (uint64_t)(int64_t)fsr;
    if (fsr == -ENOENT && !(flags & O_CREAT)) return (uint64_t)-ENOENT;

    // Virtual path — dispatch to backend; permission already checked by fs_lookup.
    if (fsr == 0 && fsn.is_virtual) {
        if (path[0]=='/' && path[1]=='p') {
            // /proc
            f = proc_open(path);
            if (!f) return (uint64_t)-ENOENT;
            goto got_file;
        }
        // /dev — dispatch by device name
        const char* dev = path + 5; // skip "/dev/"
        uint8_t match_kbd    = (dev[0]=='k' && dev[1]=='b' && dev[2]=='d' && dev[3]=='\0');
        uint8_t match_kbdraw = (dev[0]=='k' && dev[1]=='b' && dev[2]=='d' && dev[3]=='r'
                             && dev[4]=='a' && dev[5]=='w' && dev[6]=='\0');
        uint8_t match_vga    = (dev[0]=='v' && dev[1]=='g' && dev[2]=='a' && dev[3]=='\0');
        uint8_t match_mouse  = (dev[0]=='m' && dev[1]=='o' && dev[2]=='u' && dev[3]=='s'
                             && dev[4]=='e' && dev[5]=='\0');
        uint8_t match_dsp    = (dev[0]=='d' && dev[1]=='s' && dev[2]=='p' && dev[3]=='\0');
        uint8_t match_tty    = (dev[0]=='t' && dev[1]=='t' && dev[2]=='y' && dev[3]=='\0');
        uint8_t match_tty0   = (dev[0]=='t' && dev[1]=='t' && dev[2]=='y' && dev[3]=='0' && dev[4]=='\0');
        uint8_t match_null   = (dev[0]=='n' && dev[1]=='u' && dev[2]=='l' && dev[3]=='l' && dev[4]=='\0');
        uint8_t match_zero   = (dev[0]=='z' && dev[1]=='e' && dev[2]=='r' && dev[3]=='o' && dev[4]=='\0');
        uint8_t match_urnd   = (dev[0]=='u' && dev[1]=='r' && dev[2]=='a' && dev[3]=='n'
                             && dev[4]=='d' && dev[5]=='o' && dev[6]=='m' && dev[7]=='\0');
        uint8_t match_event0 = (dev[0]=='i' && dev[1]=='n' && dev[2]=='p' && dev[3]=='u'
                             && dev[4]=='t' && dev[5]=='/' && dev[6]=='e' && dev[7]=='v'
                             && dev[8]=='e' && dev[9]=='n' && dev[10]=='t' && dev[11]=='0'
                             && dev[12]=='\0');
        if      (match_tty)    f = tty_open(0);
        else if (match_tty0)   f = tty_open(0);
        else if (match_kbdraw) f = vfs_kbdraw_open();
        else if (match_event0) f = evdev_open();
        else if (match_kbd)    f = vfs_kbd_open();
        else if (match_vga)    f = vfs_vga_open();
        else if (match_mouse)  f = vfs_mouse_open();
        else if (match_dsp)    f = vfs_dsp_open();
        else if (match_null)   f = vfs_null_open();
        else if (match_zero)   f = vfs_zero_open();
        else if (match_urnd)   f = vfs_urandom_open();
        else return (uint64_t)-ENOENT;
        // fall through to got_file
    }

    if (!f) {
        // ext2 path.  fsn.inode_nr is already resolved by fs_lookup above —
        // no second ext2_lookup_path call needed.
        if (fsr == 0) {
            // File exists.  O_EXCL already handled above.
            (void)fsn.inode_nr; // inode known; ext2_open re-opens by path (stateless)
        } else {
            // fsr == -ENOENT && O_CREAT: check write+exec on parent directory.
            char parent[512];
            uint64_t plen = 0;
            while (path[plen]) plen++;
            uint64_t slash = plen;
            while (slash > 0 && path[slash] != '/') slash--;
            if (slash == 0) { parent[0] = '/'; parent[1] = '\0'; }
            else { __builtin_memcpy(parent, path, slash); parent[slash] = '\0'; }
            // Use fs_lookup for the parent too — avoids a raw ext2_lookup_path.
            fs_node_t par_fsn;
            int par_r = fs_lookup(parent, &g_current->cred,
                                   ACL_PERM_WRITE | ACL_PERM_EXEC, &par_fsn);
            if (par_r != 0) return (uint64_t)(int64_t)par_r;
        }

        f = ext2_open(path);
        if (!f && (flags & O_CREAT)) {
            mode &= ~(uint64_t)g_current->umask;
            (void)mode;
            if (!ext2_create(path)) return (uint64_t)-EIO;
            f = ext2_open(path);
        }
    }

    if (!f) return (uint64_t)-ENOENT;

got_file:
    // ── unveil check ─────────────────────────────────────────────────────
    // Paths not in the unveil table return ENOENT (as if they don't exist).
    // /dev/ and /proc/ are exempt (virtual, always accessible via this path).
    if (g_current->unveil.count > 0 &&
        !(path[1]=='d' && path[2]=='e' && path[3]=='v') &&
        !(path[1]=='p' && path[2]=='r' && path[3]=='o' && path[4]=='c')) {
        uint8_t need_u = UNVEIL_READ;
        int oflags_acc = (int)(flags & 3);
        if (oflags_acc == O_WRONLY || oflags_acc == O_RDWR) need_u |= UNVEIL_WRITE;
        if (flags & O_CREAT)                                 need_u |= UNVEIL_CREATE;
        if (!unveil_check(&g_current->unveil, path, need_u)) {
            vfs_close(f);
            return (uint64_t)-ENOENT;
        }
    }

    // ── Stamp fd rights from open flags ───────────────────────────────────
    f->rights = rights_from_oflags((int)(flags & 3), 0 /* exec_ok: not checked here */);

    // Enforce access mode: strip write for O_RDONLY (belt-and-suspenders).
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
    // Use pmm_ref_dec (CoW-aware): frame only freed when refcount→0.
    phys_addr_t pml4 = vmm_current_pml4();
    for (virt_addr_t page = new_brk; page < old_brk; page += PAGE_SIZE) {
        phys_addr_t frame;
        if (vmm_page_unmap(pml4, page, &frame))
            pmm_ref_dec(frame);
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
// Reparent direct children to g_init_task, drop the fd table, then zombify.
//
// Releasing files_shared here (matching Linux exit_files()) is load-bearing:
// peers on the other end of a unix socket must see POLLHUP the moment the
// task calls exit(), not whenever the parent eventually calls wait(). Without
// this, a process that exits while its parent is busy (e.g. makaterm exiting
// while its parent loops on something else) leaves its sockets half-open and
// the peer never learns the client is gone. The zombie only needs to retain
// exit_code / pid for waitpid — fds are not part of that contract.
static uint64_t sys_exit(uint64_t code) {
    if (g_current) {
        // Move all children to init's children list and update their ppid.
        task_t* child = g_current->children;
        while (child) {
            task_t* next_child = child->child_next;
            if (g_init_task && g_init_task != g_current) {
                child->ppid = g_init_task->pid;
                task_child_add(g_init_task, child);
            } else {
                child->ppid = 0;
                child->child_next = NULL;
            }
            child = next_child;
        }
        g_current->children = NULL;

        // Drop the fd table now so peers (pipes, sockets, ttys) see EOF
        // immediately rather than at reap time.
        if (g_current->files_shared) {
            task_files_release(g_current->files_shared);
            g_current->files_shared = NULL;
        }

        g_current->exit_code = (int32_t)(int)code;
        g_current->state = TASK_ZOMBIE;
        sched_add_zombie(g_current);

        // Wake parent and deliver SIGCHLD so it can reap background jobs.
        task_t* parent = sched_find_pid(g_current->ppid);
        if (parent) {
            signal_send(parent, SIGCHLD);
            if (parent->state == TASK_SLEEPING)
                sched_wake(parent);
        }
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
    task_child_add(g_current, child);
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
            __builtin_memcpy(ks, s, l);
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
            __builtin_memcpy(ks, s, l);
            ks[l] = '\0';
            k_envp[envc++] = ks;
        }
    }
    k_envp[envc] = NULL;

    // ── Execute permission check (path-aware walk + exec bit) ────────────
    {
        int exec_err = 0;
        uint32_t exec_ino = ext2_lookup_path(resolved, &g_current->cred, &exec_err);
        if (!exec_ino) { if (exec_err) return (uint64_t)(int64_t)exec_err; goto enoent; }
        ext2_inode_t exec_inode;
        if (!ext2_read_inode(exec_ino, &exec_inode)) goto enoent;
        inode_perm_t exec_ip = {
            .uid = exec_inode.i_uid, .gid = exec_inode.i_gid,
            .mode = exec_inode.i_mode & 0x1FF,
            .inode_nr = exec_ino, .dev = 0, .nosuid = 0,
        };
        uint32_t setuid_uid = 0xFFFFFFFFu;
        if (vfs_check_exec(&exec_ip, &g_current->cred, &setuid_uid) != 0)
            goto enoent;
    }

    // ── Load ELF ──────────────────────────────────────────────────────────
    vfs_file_t* f = ext2_open(resolved);
    if (!f) { goto enoent; }

    const uint64_t MAX_ELF = 8ULL * 1024ULL * 1024ULL; // 8 MiB
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
    vmm_free_user_ex(g_current->mm_shared->pml4_phys, g_current->mm_shared->mm);
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
// spawn(path_ptr, argv_ptr, envp_ptr, stdio_ptr, attr_ptr) → child pid, -errno.
//
// Loads an ELF from the given absolute ext2 path into a brand-new address
// space and schedules it.  Parent returns immediately with the child's pid.
//
// path_ptr  — user pointer to NUL-terminated absolute path string
// argv_ptr  — user pointer to NULL-terminated array of user char* (or 0 → {path, NULL})
// envp_ptr  — user pointer to NULL-terminated array of user char* (or 0 → default env)
// stdio_ptr — user pointer to int[3]: stdio fd specs for child's 0/1/2
//               -1  = inherit that fd from parent
//               -2  = /dev/null
//               >=0 = dup that specific parent fd
//               0 (null ptr) = open /dev/tty0 for all three
// attr_ptr  — user pointer to spawn_attr_t, or 0 for no extra attributes
static uint64_t sys_spawn(uint64_t path_ptr, uint64_t argv_ptr,
                           uint64_t envp_ptr, uint64_t stdio_ptr,
                           uint64_t attr_ptr) {
    serial_puts_dbg("[spawn] path="); serial_puts_dbg((const char*)path_ptr); serial_putc_dbg('\n');
    if (!g_current) return (uint64_t)-EINVAL;
    if (!path_ptr)  return (uint64_t)-EINVAL;

    // ── Copy path ─────────────────────────────────────────────────────────
    char path[256];
    const char* upath = (const char*)path_ptr;
    uint64_t plen = 0;
    while (plen < 255 && upath[plen]) { path[plen] = upath[plen]; plen++; }
    path[plen] = '\0';
    if (plen == 0) return (uint64_t)-EINVAL;

    // ── Copy argv (up to 64 args) ─────────────────────────────────────────
    #define SPAWN_MAX_ARGS 64
    #define SPAWN_MAX_ARG_LEN 512
    char* k_argv[SPAWN_MAX_ARGS + 1];
    uint32_t argc = 0;
    if (argv_ptr) {
        const uint64_t* uargv = (const uint64_t*)argv_ptr;
        while (argc < SPAWN_MAX_ARGS) {
            uint64_t ustr = uargv[argc];
            if (!ustr) break;
            const char* s = (const char*)ustr;
            uint64_t l = 0; while (l < SPAWN_MAX_ARG_LEN && s[l]) l++;
            char* ks = kmalloc(l + 1);
            if (!ks) goto oom_argv;
            __builtin_memcpy(ks, s, l);
            ks[l] = '\0';
            k_argv[argc++] = ks;
        }
    }
    k_argv[argc] = NULL;
    // If caller passed no argv, synthesise {path, NULL}.
    if (argc == 0) { k_argv[0] = path; k_argv[1] = NULL; argc = 1; }

    // ── Copy envp (up to 64 vars) ─────────────────────────────────────────
    #define SPAWN_MAX_ENVS 64
    char* k_envp[SPAWN_MAX_ENVS + 1];
    uint32_t envc = 0;
    if (envp_ptr) {
        const uint64_t* uenvp = (const uint64_t*)envp_ptr;
        while (envc < SPAWN_MAX_ENVS) {
            uint64_t ustr = uenvp[envc];
            if (!ustr) break;
            const char* s = (const char*)ustr;
            uint64_t l = 0; while (l < SPAWN_MAX_ARG_LEN && s[l]) l++;
            char* ks = kmalloc(l + 1);
            if (!ks) goto oom_envp;
            __builtin_memcpy(ks, s, l);
            ks[l] = '\0';
            k_envp[envc++] = ks;
        }
    }
    k_envp[envc] = NULL;
    // Default env if caller passed nothing.
    const char* def_envp[] = { "PATH=/bin", "HOME=/root", "TERM=linux", "PWD=/", NULL };
    const char* const* final_envp = envc ? (const char* const*)k_envp : def_envp;

    // ── Resolve stdio spec ────────────────────────────────────────────────
    // stdio_ptr points to int[3]; 0 = open tty0 for all.
    int stdio[3] = { -1, -1, -1 }; // default: inherit
    if (stdio_ptr) {
        const int* us = (const int*)stdio_ptr;
        stdio[0] = us[0]; stdio[1] = us[1]; stdio[2] = us[2];
    }

    // ── Launch ────────────────────────────────────────────────────────────
    uint32_t pid = pid_alloc();
    if (!pid) goto oom_envp;

    task_t* child = elf_exec_from_ext2(path, pid,
                                        (const char* const*)k_argv,
                                        final_envp, stdio);
    if (!child) {
        pid_free(pid);
        for (uint32_t i = 0; i < envc; i++) kfree(k_envp[i]);
        if (argv_ptr) for (uint32_t i = 0; i < argc; i++) kfree(k_argv[i]);
        return (uint64_t)-ENOENT;
    }

    // Wire parent-child relationship so waitpid works correctly.
    child->ppid = g_current->pid;

    // ── Apply spawn_attr if provided ──────────────────────────────────────
    // spawn_attr_t is now ~32 bytes (unveil is a separate pointer), so it's
    // safe to copy directly onto the kernel stack.
    if (attr_ptr) {
        if (!_access_ok(attr_ptr, sizeof(spawn_attr_t))) goto bad_attr;
        spawn_attr_t a;
        __builtin_memcpy(&a, (const void*)attr_ptr, sizeof(spawn_attr_t));

        if (a.flags & SPAWN_ATTR_CRED) {
            child->cred.ruid = a.uid;
            child->cred.euid = a.uid;
            child->cred.suid = a.uid;
            child->cred.rgid = a.gid;
            child->cred.egid = a.gid;
            child->cred.sgid = a.gid;
        }
        if (a.flags & SPAWN_ATTR_UMASK) {
            child->umask = a.umask & 0777u;
        }
        if (a.flags & SPAWN_ATTR_PLEDGE) {
            child->pledge_mask = pledge_restrict(g_current->pledge_mask,
                                                 a.pledge_mask);
        }
        if ((a.flags & SPAWN_ATTR_UNVEIL) && a.nunveil && a.unveil) {
            uint32_t n = a.nunveil;
            uint64_t uptr = (uint64_t)(uintptr_t)a.unveil;
            if (!_access_ok(uptr, (uint64_t)n * sizeof(spawn_unveil_entry_t)))
                goto bad_attr;
            // Copy unveil entries one page-safe chunk at a time — each entry
            // is 257 bytes; kmalloc the whole array then walk it.
            spawn_unveil_entry_t* ue = kmalloc((uint64_t)n * sizeof(spawn_unveil_entry_t));
            if (!ue) goto bad_attr;
            __builtin_memcpy(ue, a.unveil, (uint64_t)n * sizeof(spawn_unveil_entry_t));
            unveil_free(&child->unveil);
            unveil_init(&child->unveil);
            for (uint32_t i = 0; i < n; i++)
                unveil_add(&child->unveil, ue[i].path, ue[i].perms);
            unveil_lock(&child->unveil);
            kfree(ue);
        }
    }
    bad_attr:;

    // Job control: if the child inherits the parent's tty (stdio[0]==-1),
    // give the terminal to the child's process group immediately.
    // This is what a proper shell does after fork+exec: tcsetpgrp(tty, child_pgid).
    // Without this, bash detects it's not in the foreground and spins on SIGTTOU.
    if (!stdio_ptr || (((const int*)stdio_ptr)[0] == -1)) {
        tty_t* tty = tty_get_ctty();
        if (!tty) tty = &g_tty0;
        tty->fg_pgid = child->pgid;
    }

    task_child_add(g_current, child);
    sched_add(child);

    for (uint32_t i = 0; i < envc; i++) kfree(k_envp[i]);
    if (argv_ptr) for (uint32_t i = 0; i < argc; i++) kfree(k_argv[i]);
    return (uint64_t)pid;

oom_envp:
    for (uint32_t i = 0; i < envc; i++) kfree(k_envp[i]);
oom_argv:
    if (argv_ptr) for (uint32_t i = 0; i < argc; i++) kfree(k_argv[i]);
    return (uint64_t)-ENOMEM;
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
        // CoW clone: pass mm so private pages are CoW-shared, not deep-copied.
        if (!vmm_clone_user_ex(new_pml4, g_current->mm_shared->pml4_phys,
                               g_current->mm_shared->mm)) {
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

    t->tgid             = g_current->tgid;
    t->ppid             = g_current->ppid;
    t->pgid             = g_current->pgid;
    t->sid              = g_current->sid;
    t->flags            = TASK_FLAG_THREAD;
    t->state            = TASK_READY;
    t->next             = NULL;
    t->children         = NULL;
    t->child_next       = NULL;
    t->pg_prev = t->pg_next = NULL;
    t->tg_prev = t->tg_next = NULL;
    t->sid_prev = t->sid_next = NULL;
    t->home_cpu = 0;
    t->mlfq_level       = 0;
    t->mlfq_ticks_left  = 0;
    t->sigstate.pending = 0;
    t->sigstate.blocked = 0;
    t->exit_code        = 0;
    t->sleep_until_ns   = 0;
    t->umask            = g_current->umask;

    // Inherit comm, credentials, pledge, unveil, and FPU state from parent.
    t->cwd = kmalloc(KPATH_MAX);
    if (t->cwd) __builtin_memcpy(t->cwd, g_current->cwd, KPATH_MAX);
    __builtin_memcpy(t->comm, g_current->comm, sizeof(t->comm));
    t->cred         = g_current->cred;
    t->pledge_mask  = g_current->pledge_mask;
    unveil_copy(&t->unveil, &g_current->unveil);
    __asm__ volatile("fxsave %0" : "=m"(t->ctx.fxsave_buf));

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
//   pid > 0 : wait for that specific child (must be a direct child).
//   pid == -1: wait for any direct child.
//   options & WNOHANG: return 0 immediately if no child ready.
// Returns reaped child pid, 0 (WNOHANG + not ready), or -errno.
//
// Uses the per-task children list instead of a global queue walk.
static uint64_t sys_wait(uint64_t pid_arg, uint64_t status_ptr, uint64_t options) {
    if (!g_current) return (uint64_t)-EINVAL;

    int64_t  signed_pid = (int64_t)pid_arg;
    uint32_t target_pid = (signed_pid == -1) ? 0 : (uint32_t)signed_pid;

    for (;;) {
        // No children at all → ECHILD.
        if (!g_current->children) return (uint64_t)-ECHILD;

        // Walk children list looking for a zombie to reap.
        task_t*  prev        = NULL;
        task_t*  child       = g_current->children;
        task_t*  zombie      = NULL;
        task_t*  zombie_prev = NULL;
        uint8_t  found_target = 0;

        while (child) {
            if (target_pid == 0 || child->pid == target_pid) {
                found_target = 1;
                if (child->state == TASK_ZOMBIE && !zombie) {
                    zombie      = child;
                    zombie_prev = prev;
                }
            }
            prev  = child;
            child = child->child_next;
        }

        // Specific pid not in our children list → ECHILD.
        if (target_pid != 0 && !found_target) return (uint64_t)-ECHILD;

        if (zombie) {
            // Remove from children list.
            if (zombie_prev) zombie_prev->child_next = zombie->child_next;
            else             g_current->children     = zombie->child_next;
            zombie->child_next = NULL;

            // Also remove from global zombie list.
            sched_reap_zombie(zombie->pid);

            int32_t  code       = zombie->exit_code;
            uint32_t child_pid  = zombie->pid;
            uint32_t child_pgid = zombie->pgid;

            // Job control: return terminal to parent's process group.
            tty_t* tty = tty_get_ctty();
            if (!tty) tty = &g_tty0;
            if (tty && tty->fg_pgid == child_pgid)
                tty->fg_pgid = g_current->pgid;

            if (status_ptr) {
                int* sp = (int*)(uintptr_t)status_ptr;
                *sp = (int)((code & 0xFF) << 8);
            }

            process_destroy(zombie);
            return (uint64_t)child_pid;
        }

        // No zombie yet.
        if (options & WNOHANG) return 0;

        // Block until a child exits and wakes us (see sys_exit / signal.c).
        sched_sleep();
    }
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
    if (pathlen == 0 || pathlen > 511) return (uint64_t)-EINVAL;
    if (max_entries > 256) max_entries = 256;

    char* raw  = kmalloc(512);
    char* path = kmalloc(512);
    if (!raw || !path) { kfree(raw); kfree(path); return (uint64_t)-ENOMEM; }

    const char* upath = (const char*)path_ptr;
    __builtin_memcpy(raw, upath, pathlen);
    raw[pathlen] = '\0';

    if (raw[0] != '/') {
        const char* cwd = g_current ? g_current->cwd : "/";
        uint64_t clen = 0; while (cwd[clen]) clen++;
        uint64_t j = 0;
        for (; j < clen && j < 510; j++) path[j] = cwd[j];
        if (j > 0 && path[j-1] != '/') path[j++] = '/';
        for (uint64_t k = 0; raw[k] && j < 511; k++, j++) path[j] = raw[k];
        path[j] = '\0';
    } else {
        for (uint64_t k = 0; k <= pathlen; k++) path[k] = raw[k];
    }
    kfree(raw);

    ext2_entry_t* kbuf = kmalloc(max_entries * sizeof(ext2_entry_t));
    if (!kbuf) { kfree(path); return (uint64_t)-ENOMEM; }

    int count;
    {
        fs_node_t fsn;
        int fsr = fs_lookup(path, &g_current->cred, ACL_PERM_READ | ACL_PERM_EXEC, &fsn);
        if (fsr != 0) { kfree(kbuf); kfree(path); return (uint64_t)(int64_t)fsr; }
        if (fsn.type != FS_TYPE_DIR) { kfree(kbuf); kfree(path); return (uint64_t)-ENOTDIR; }

        if (fsn.is_virtual) {
            if (path[0]=='/' && path[1]=='p') {
                count = proc_readdir(path, kbuf, (int)max_entries);
            } else {
                count = dev_readdir(kbuf, (int)max_entries);
            }
        } else {
            // ext2: permissions already checked by fs_lookup above.
            count = ext2_readdir(path, kbuf, (int)max_entries);
            if (count < 0) { kfree(kbuf); kfree(path); return (uint64_t)-ENOENT; }
            // Inject virtual top-level directories into root listing.
            if (path[0] == '/' && path[1] == '\0') {
                static const char* virt_dirs[] = { "proc", "dev", NULL };
                for (int v = 0; virt_dirs[v] && count < (int)max_entries; v++) {
                    int already = 0;
                    for (int j = 0; j < count; j++) {
                        const char* a = kbuf[j].name; const char* b = virt_dirs[v];
                        while (*a && *b && *a == *b) { a++; b++; }
                        if (*a == '\0' && *b == '\0') { already = 1; break; }
                    }
                    if (!already) {
                        const char* n = virt_dirs[v];
                        int ni = 0;
                        while (n[ni]) { kbuf[count].name[ni] = n[ni]; ni++; }
                        kbuf[count].name[ni]     = '\0';
                        kbuf[count].inode_num    = 0xF0000000 + (uint32_t)v;
                        kbuf[count].size         = 0;
                        kbuf[count].is_dir       = 1;
                        count++;
                    }
                }
            }
        }
    }
    if (count < 0) { kfree(kbuf); kfree(path); return (uint64_t)-ENOENT; }

    ext2_entry_t* ubuf = (ext2_entry_t*)buf_ptr;
    __builtin_memcpy(ubuf, kbuf, (uint64_t)count * sizeof(ext2_entry_t));

    kfree(kbuf);
    kfree(path);
    return (uint64_t)count;
}

// Forward declarations for copy helpers (defined later in this file).
static int copy_to_user(void* dst_u, const void* src, uint64_t len);
static int copy_from_user(void* dst, const void* src_u, uint64_t len);

// ── sys_stat ──────────────────────────────────────────────────────────────
// stat(path_ptr, pathlen, stat_ptr) → 0 or -errno
// Fills a POSIX struct stat in userspace directly.
static uint64_t sys_stat(uint64_t path_ptr, uint64_t pathlen, uint64_t stat_ptr) {
    if (!g_current || pathlen == 0 || pathlen > 511 || !stat_ptr) return (uint64_t)-EINVAL;

    char raw[512];
    const char* upath = (const char*)path_ptr;
    __builtin_memcpy(raw, upath, pathlen);
    raw[pathlen] = '\0';

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
        for (uint64_t k = 0; k <= pathlen; k++) path[k] = raw[k];
    }

    struct stat kst;
    __builtin_memset(&kst, 0, sizeof(kst));

    fs_node_t fsn;
    // stat needs no permission on the file itself — only parent dir traversal,
    // which ext2_lookup_path already checks on every component.
    int fsr = fs_lookup(path, &g_current->cred, 0, &fsn);
    if (fsr != 0) return (uint64_t)(int64_t)fsr;

    if (fsn.is_virtual) {
        // All virtual nodes (both /dev and /proc) now have per-node
        // uid/gid/mode from fs_lookup → virtfs_lookup → virt_resolve.
        kst.st_ino     = fsn.inode_nr ? fsn.inode_nr : 0xE0000001;
        kst.st_mode    = fsn.mode;
        kst.st_nlink   = (fsn.type == FS_TYPE_DIR) ? 2 : 1;
        kst.st_uid     = fsn.uid;
        kst.st_gid     = fsn.gid;
        kst.st_blksize = 4096;
    } else {
        kst.st_ino     = fsn.inode_nr;
        kst.st_mode    = fsn.mode;
        kst.st_size    = (int64_t)fsn.size;
        kst.st_uid     = fsn.uid;
        kst.st_gid     = fsn.gid;
        kst.st_nlink   = (fsn.type == FS_TYPE_DIR) ? 2 : 1;
        kst.st_blksize = 4096;
        kst.st_blocks  = (int64_t)((fsn.size + 511) / 512);
    }
    return (copy_to_user((void*)stat_ptr, &kst, sizeof(kst)) == 0) ? 0 : (uint64_t)-EFAULT;
}

// ── sys_unlink ────────────────────────────────────────────────────────────
// unlink(path_ptr, pathlen) → 0 or -1
static uint64_t sys_unlink(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current || pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;

    char path[256];
    const char* upath = (const char*)path_ptr;
    __builtin_memcpy(path, upath, pathlen);
    path[pathlen] = '\0';

    // Check write permission on the file's parent directory.
    // Find parent path.
    uint64_t last = 0;
    for (uint64_t i = 0; i < pathlen; i++) if (path[i] == '/') last = i;
    char parent[256];
    if (last == 0) { parent[0] = '/'; parent[1] = '\0'; }
    else { __builtin_memcpy(parent, path, last); parent[last] = '\0'; }

    int ul_err = 0;
    uint32_t par_ino = ext2_lookup_path(parent, &g_current->cred, &ul_err);
    if (!par_ino) return ul_err ? (uint64_t)(int64_t)ul_err : (uint64_t)-ENOENT;
    ext2_inode_t par_inode;
    if (!ext2_read_inode(par_ino, &par_inode)) return (uint64_t)-ENOENT;
    inode_perm_t par_ip = {
        .uid = par_inode.i_uid, .gid = par_inode.i_gid,
        .mode = par_inode.i_mode & 0x1FF,
        .inode_nr = par_ino, .dev = 0, .nosuid = 0,
    };
    int upr = vfs_check_perm(&par_ip, &g_current->cred, ACL_PERM_WRITE | ACL_PERM_EXEC);
    if (upr != 0) return (uint64_t)(int64_t)upr;

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
    __builtin_memcpy(src, usrc, srclen);
    src[srclen] = '\0';
    __builtin_memcpy(dst, udst, dstlen);
    dst[dstlen] = '\0';

    // Require write+exec on src parent directory.
    uint64_t src_last = 0;
    for (uint64_t i = 0; i < srclen; i++) if (src[i] == '/') src_last = i;
    char src_parent[256];
    if (src_last == 0) { src_parent[0] = '/'; src_parent[1] = '\0'; }
    else { __builtin_memcpy(src_parent, src, src_last); src_parent[src_last] = '\0'; }

    int rn_err = 0;
    uint32_t sp_ino = ext2_lookup_path(src_parent, &g_current->cred, &rn_err);
    if (!sp_ino) return rn_err ? (uint64_t)(int64_t)rn_err : (uint64_t)-ENOENT;
    ext2_inode_t sp_inode;
    if (!ext2_read_inode(sp_ino, &sp_inode)) return (uint64_t)-ENOENT;
    inode_perm_t sp_ip = {
        .uid = sp_inode.i_uid, .gid = sp_inode.i_gid,
        .mode = sp_inode.i_mode & 0x1FF,
        .inode_nr = sp_ino, .dev = 0, .nosuid = 0,
    };
    int spr = vfs_check_perm(&sp_ip, &g_current->cred, ACL_PERM_WRITE | ACL_PERM_EXEC);
    if (spr != 0) return (uint64_t)(int64_t)spr;

    return ext2_rename(src, dst) ? 0 : (uint64_t)-ENOENT;
}

// ── sys_getcwd ────────────────────────────────────────────────────────────
// getcwd(buf_ptr, buflen) → buf_ptr on success, -errno on error.
// Linux extension: buf_ptr==NULL && buflen==0 → kernel allocates via mmap,
// returns pointer to user-space buffer (caller must free with munmap).
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t buflen) {
    if (!g_current) return (uint64_t)-EINVAL;

    const char* cwd = g_current->cwd;
    uint64_t cwdlen = 0;
    while (cwd[cwdlen]) cwdlen++;
    cwdlen++; // include NUL

    // Linux extension: NULL buf with size 0 → kernel allocates a buffer.
    // We extend the heap (brk) by one page so the returned address is always
    // in the low user address space (< 0x80000000).  Bash does movslq on the
    // return value; addresses above 2GB would be sign-extended to negative.
    // We also eagerly allocate and map the physical frame (kernel writes to the
    // buffer before returning, so demand-paging from kernel context is avoided).
    if (!buf_ptr && buflen == 0) {
        mm_t* mm = g_current->mm_shared->mm;

        // Allocate a physical frame for the buffer.
        phys_addr_t frame = pmm_buddy_alloc(0);
        if (frame == PMM_INVALID_ADDR) return (uint64_t)-ENOMEM;

        // Use current brk as the buffer VA (it's in the low 2GB for typical ELFs).
        virt_addr_t uva = (mm->brk + PAGE_MASK) & ~PAGE_MASK; // page-align up
        virt_addr_t uva_end = uva + PAGE_SIZE;

        // Extend heap VMA to cover the new page.
        virt_addr_t heap_start = mm->brk_start;
        vma_t* prev = NULL;
        vma_t* v = mm->vmas;
        while (v) {
            if (v->start == heap_start) {
                if (prev) prev->next = v->next; else mm->vmas = v->next;
                kfree(v);
                break;
            }
            prev = v; v = v->next;
        }
        if (!mm_vma_add(mm, heap_start, uva_end, VMA_R | VMA_W | VMA_ANON)) {
            pmm_buddy_free(frame, 0);
            return (uint64_t)-ENOMEM;
        }
        mm->brk = uva_end;

        // Eagerly map the frame (kernel writes into it next).
        uint64_t pte_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX;
        if (!vmm_page_map(vmm_current_pml4(), uva, frame, pte_flags)) {
            pmm_buddy_free(frame, 0);
            return (uint64_t)-ENOMEM;
        }

        // Zero the frame and write the cwd string.
        uint8_t* kbuf = (uint8_t*)(frame + HHDM_OFFSET);
        __builtin_memset(kbuf, 0, PAGE_SIZE);
        __builtin_memcpy(kbuf, cwd, cwdlen);

        return uva;
    }

    if (!buf_ptr || buflen == 0) return (uint64_t)-EINVAL;
    if (cwdlen > buflen) return (uint64_t)-ERANGE;

    char* ubuf = (char*)buf_ptr;
    __builtin_memcpy(ubuf, cwd, cwdlen);
    return buf_ptr;
}

// ── sys_chdir ─────────────────────────────────────────────────────────────
// chdir(path_ptr, pathlen) → 0 or -1
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current || pathlen == 0 || pathlen > 511) return (uint64_t)-EINVAL;

    char raw[512];
    const char* upath = (const char*)path_ptr;
    __builtin_memcpy(raw, upath, pathlen);
    raw[pathlen] = '\0';

    // Resolve relative paths against cwd.
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
        for (uint64_t k = 0; k <= pathlen; k++) path[k] = raw[k];
    }

    // Unified lookup — covers both virtfs and ext2.
    {
        fs_node_t fsn;
        int fsr = fs_lookup(path, &g_current->cred, ACL_PERM_EXEC, &fsn);
        if (fsr != 0) return (uint64_t)(int64_t)fsr;
        if (fsn.type != FS_TYPE_DIR) return (uint64_t)-ENOTDIR;
        if (fsn.is_virtual) {
            uint32_t len = 0;
            while (len < KPATH_MAX - 1 && path[len]) { g_current->cwd[len] = path[len]; len++; }
            g_current->cwd[len] = '\0';
            return 0;
        }
    }

    // ext2: fs_lookup already verified existence, type, and permissions.
    uint32_t len = 0;
    while (len < KPATH_MAX - 1 && path[len]) { g_current->cwd[len] = path[len]; len++; }
    g_current->cwd[len] = '\0';
    return 0;
}

// ── sys_mkdir ─────────────────────────────────────────────────────────────
// mkdir(path_ptr, pathlen) → 0 or -1
static uint64_t sys_mkdir(uint64_t path_ptr, uint64_t pathlen) {
    if (!g_current || pathlen == 0 || pathlen > 255) return (uint64_t)-EINVAL;

    char path[256];
    const char* upath = (const char*)path_ptr;
    __builtin_memcpy(path, upath, pathlen);
    path[pathlen] = '\0';

    // Require write+exec on parent directory.
    uint64_t mk_last = 0;
    for (uint64_t i = 0; i < pathlen; i++) if (path[i] == '/') mk_last = i;
    char mk_parent[256];
    if (mk_last == 0) { mk_parent[0] = '/'; mk_parent[1] = '\0'; }
    else { __builtin_memcpy(mk_parent, path, mk_last); mk_parent[mk_last] = '\0'; }

    int mk_err = 0;
    uint32_t mkp_ino = ext2_lookup_path(mk_parent, &g_current->cred, &mk_err);
    if (!mkp_ino) return mk_err ? (uint64_t)(int64_t)mk_err : (uint64_t)-ENOENT;
    ext2_inode_t mkp_inode;
    if (!ext2_read_inode(mkp_ino, &mkp_inode)) return (uint64_t)-ENOENT;
    inode_perm_t mkp_ip = {
        .uid = mkp_inode.i_uid, .gid = mkp_inode.i_gid,
        .mode = mkp_inode.i_mode & 0x1FF,
        .inode_nr = mkp_ino, .dev = 0, .nosuid = 0,
    };
    int mkpr = vfs_check_perm(&mkp_ip, &g_current->cred, ACL_PERM_WRITE | ACL_PERM_EXEC);
    if (mkpr != 0) return (uint64_t)(int64_t)mkpr;

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
    g_current->files_shared->fd_flags[newfd] = 0;  // POSIX: dup2 clears FD_CLOEXEC
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
// Supports MAP_PRIVATE | MAP_ANONYMOUS, MAP_SHARED | MAP_ANONYMOUS,
// and MAP_SHARED with fd (shmem fd from shm_open).  MAP_FIXED is supported.
static uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t off) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;
    if (!len) return (uint64_t)-EINVAL;

    int is_anon   = !!(flags & MAP_ANONYMOUS);
    int is_shared = !!(flags & MAP_SHARED);
    int has_fd    = ((int64_t)fd != -1);

    // Validate flag combinations.
    // MAP_SHARED | MAP_ANONYMOUS → anonymous shared (for fork-inherited shm)
    // MAP_SHARED with fd         → named shmem (shm_open fd)
    // MAP_PRIVATE | MAP_ANONYMOUS → private anonymous (existing behavior)
    // MAP_PRIVATE with fd        → not supported yet (file-backed mmap)
    if (!is_anon && !has_fd) return (uint64_t)-EINVAL;
    if (!is_shared && has_fd) return (uint64_t)-ENOSYS; // MAP_PRIVATE + fd: TODO
    if (is_anon && has_fd) return (uint64_t)-EINVAL;    // anon + fd: nonsensical

    mm_t* mm = g_current->mm_shared->mm;

    // Round len up to page boundary.
    len = (len + PAGE_MASK) & ~PAGE_MASK;
    uint32_t npages = (uint32_t)(len / PAGE_SIZE);

    virt_addr_t vaddr;
    if (flags & MAP_FIXED) {
        if (!addr || (addr & PAGE_MASK)) return (uint64_t)-EINVAL;
        // Unmap anything in the range first (POSIX requirement).
        phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
        for (virt_addr_t p = addr; p < addr + len; p += PAGE_SIZE) {
            phys_addr_t frame;
            if (vmm_page_unmap(pml4, p, &frame)) {
                // Only free the frame if the VMA at this address is private.
                // CoW-aware: pmm_ref_dec only frees when refcount→0.
                vma_t* old_vma = mm_vma_find(mm, p);
                if (!old_vma || !old_vma->shmem)
                    pmm_ref_dec(frame);
            }
        }
        mm_vma_remove(mm, addr, addr + len);
        vaddr = addr;
    } else if (addr) {
        addr &= ~PAGE_MASK;
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
    if (is_shared) vma_flags |= VMA_SHARED;

    if (!mm_vma_add(mm, vaddr, vaddr + len, vma_flags))
        return (uint64_t)-ENOMEM;

    // ── Attach shmem backing ─────────────────────────────────────────────
    if (is_shared) {
        shmem_t* shm = NULL;

        if (has_fd) {
            // MAP_SHARED with fd: the fd must be a shmem fd.
            if (fd >= g_current->files_shared->fd_capacity)
                goto fail_unmap;
            vfs_file_t* f = g_current->files_shared->fd_table[fd];
            if (!f) goto fail_unmap;
            // Identify shmem fds by checking for the shmem_close callback.
            // (set when shm_open creates the fd — see shmem fd implementation)
            extern void shmem_fd_close(vfs_file_t*);
            if (f->close != shmem_fd_close) {
                // Not a shmem fd.
                goto fail_unmap;
            }
            shm = (shmem_t*)f->ctx;
            if (!shm) goto fail_unmap;

            // Validate offset and size.
            uint32_t pg_off = (uint32_t)(off / PAGE_SIZE);
            if (pg_off + npages > shm->npages) goto fail_unmap;

            shmem_ref(shm);

            vma_t* vma = mm_vma_find(mm, vaddr);
            if (vma) {
                vma->shmem       = shm;
                vma->shmem_pgoff = pg_off;
            }
        } else {
            // MAP_SHARED | MAP_ANONYMOUS: create a new anonymous shmem.
            shm = shmem_create(npages,
                               g_current->cred.euid,
                               g_current->cred.egid,
                               0600);
            if (!shm) goto fail_unmap;

            vma_t* vma = mm_vma_find(mm, vaddr);
            if (vma) {
                vma->shmem       = shm;
                vma->shmem_pgoff = 0;
            }
            // shm starts with refcount=1 (owned by this VMA)
        }
    }

    // Pages are demand-paged — do not allocate physical frames here.
    return vaddr;

fail_unmap:
    mm_vma_remove(mm, vaddr, vaddr + len);
    return (uint64_t)-EINVAL;
}

// ── sys_munmap ────────────────────────────────────────────────────────────
static uint64_t sys_munmap(uint64_t addr, uint64_t len) {
    if (!g_current || !g_current->mm_shared->mm) return (uint64_t)-EINVAL;
    if (!addr || (addr & PAGE_MASK))              return (uint64_t)-EINVAL;
    if (!len)                                     return (uint64_t)-EINVAL;

    len = (len + PAGE_MASK) & ~PAGE_MASK;
    phys_addr_t pml4 = g_current->mm_shared->pml4_phys;
    mm_t* mm = g_current->mm_shared->mm;

    // Unmap physical pages.  Only free frames for private VMAs —
    // shared frames are owned by the shmem_t and freed when its
    // refcount drops to zero.
    for (virt_addr_t p = addr; p < addr + len; p += PAGE_SIZE) {
        vma_t* vma = mm_vma_find(mm, p);
        phys_addr_t frame;
        if (vmm_page_unmap(pml4, p, &frame)) {
            if (!vma || !vma->shmem)
                pmm_ref_dec(frame);  // CoW-aware: only frees when rc→0
            // Shared frames: just unmap the PTE, don't free the physical page.
        }
    }

    // Remove VMA descriptors covering the range (this also unrefs shmem).
    mm_vma_remove(mm, addr, addr + len);
    return 0;
}

// ── sys_shm_open ─────────────────────────────────────────────────────────
// shm_open(name, namelen, oflags, mode) → fd or -errno
//
// POSIX shared memory: opens (or creates) a named shmem object and returns
// a file descriptor suitable for mmap() and ftruncate().
//
// oflags: O_CREAT, O_EXCL, O_RDONLY/O_WRONLY/O_RDWR, O_TRUNC.
// mode: permission bits (only used when creating, masked by umask).
// Name must start with '/' and contain no further slashes (POSIX requirement).
static uint64_t sys_shm_open(uint64_t name_ptr, uint64_t namelen,
                              uint64_t oflags, uint64_t mode) {
    serial_puts_dbg("[shm_open] namelen="); serial_hex_dbg(namelen);
    if (!g_current || !name_ptr) { serial_puts_dbg("[shm_open] EINVAL\n"); return (uint64_t)-EINVAL; }
    if (namelen == 0 || namelen > SHMEM_NAME_MAX) return (uint64_t)-ENAMETOOLONG;

    // Copy name from userspace.
    char name[SHMEM_NAME_MAX + 1];
    const char* uname = (const char*)name_ptr;
    __builtin_memcpy(name, uname, namelen);
    name[namelen] = '\0';

    // POSIX: name must start with '/' and contain no embedded slashes.
    if (name[0] != '/') return (uint64_t)-EINVAL;
    for (uint64_t i = 1; i < namelen; i++)
        if (name[i] == '/') return (uint64_t)-EINVAL;

    // Use the part after the leading '/' as the internal name.
    const char* iname = name + 1;
    if (!iname[0]) return (uint64_t)-EINVAL; // name was just "/"

    int access_mode = (int)(oflags & 3); // O_RDONLY=0, O_WRONLY=1, O_RDWR=2
    int creating    = !!(oflags & O_CREAT);
    int exclusive   = !!(oflags & O_EXCL);
    int truncating  = !!(oflags & O_TRUNC);

    shmem_t* shm = shmem_ns_find(iname);

    if (shm) {
        // Object already exists.
        if (creating && exclusive) return (uint64_t)-EEXIST;

        // Permission check.
        int rc = shmem_check_access(shm, &g_current->cred, access_mode);
        if (rc) return (uint64_t)rc;

        // O_TRUNC: resize to 0 (only if writable).
        if (truncating && (access_mode == 1 || access_mode == 2))
            shmem_resize(shm, 0);
    } else {
        // Object does not exist.
        if (!creating) return (uint64_t)-ENOENT;

        // Apply umask to mode.
        uint16_t effective_mode = (uint16_t)(mode & ~g_current->umask & 0777);

        // Create a zero-size object (caller uses ftruncate to set size).
        shm = shmem_create(0, g_current->cred.euid, g_current->cred.egid,
                           effective_mode);
        if (!shm) return (uint64_t)-ENOMEM;

        // Copy name into the object.
        uint64_t ilen = 0;
        while (iname[ilen] && ilen < SHMEM_NAME_MAX) {
            shm->name[ilen] = iname[ilen];
            ilen++;
        }
        shm->name[ilen] = '\0';

        // Insert into namespace.  On failure, the object is orphaned — free it.
        int rc = shmem_ns_insert(shm);
        if (rc) {
            shmem_unref(shm);
            return (uint64_t)rc;
        }
    }

    // Create the fd wrapping this shmem object.
    vfs_file_t* f = shmem_fd_create(shm);
    if (!f) return (uint64_t)-ENOMEM;

    // Allocate an fd slot.
    task_files_t* files = g_current->files_shared;
    for (uint32_t i = 0; i < files->fd_capacity; i++) {
        if (!files->fd_table[i]) {
            files->fd_table[i] = f;
            files->fd_flags[i] = (oflags & O_CLOEXEC) ? 1 : 0;
            serial_puts_dbg("[shm_open] fd="); serial_hex_dbg((uint64_t)i);
            return i;
        }
    }
    // Table full — try to grow.
    if (fd_table_grow(files)) {
        for (uint32_t i = 0; i < files->fd_capacity; i++) {
            if (!files->fd_table[i]) {
                files->fd_table[i] = f;
                files->fd_flags[i] = (oflags & O_CLOEXEC) ? 1 : 0;
                return i;
            }
        }
    }
    vfs_close(f);
    return (uint64_t)-EMFILE;
}

// ── sys_shm_unlink ───────────────────────────────────────────────────────
// shm_unlink(name, namelen) → 0 or -errno
//
// Remove a named shmem object from the namespace.  The object and its
// pages persist until all fds and mmap references are dropped.
static uint64_t sys_shm_unlink(uint64_t name_ptr, uint64_t namelen) {
    if (!g_current || !name_ptr) return (uint64_t)-EINVAL;
    if (namelen == 0 || namelen > SHMEM_NAME_MAX) return (uint64_t)-ENAMETOOLONG;

    char name[SHMEM_NAME_MAX + 1];
    const char* uname = (const char*)name_ptr;
    __builtin_memcpy(name, uname, namelen);
    name[namelen] = '\0';

    if (name[0] != '/') return (uint64_t)-EINVAL;
    const char* iname = name + 1;
    if (!iname[0]) return (uint64_t)-EINVAL;

    shmem_t* shm = shmem_ns_find(iname);
    if (!shm) return (uint64_t)-ENOENT;

    // Permission check: only owner or root can unlink.
    if (g_current->cred.euid != 0 && g_current->cred.euid != shm->uid)
        return (uint64_t)-EACCES;

    // Remove from namespace (makes it invisible for future shm_open calls).
    // The shmem_t itself survives if there are still fd/VMA references.
    shmem_ns_remove(shm);
    shm->name[0] = '\0'; // prevent double-remove in shmem_unref

    // Drop the namespace's implicit reference.  If there are no fd/VMA
    // references, this will free the object immediately.
    shmem_unref(shm);
    return 0;
}

// ── sys_nanosleep ─────────────────────────────────────────────────────────
// nanosleep(req, rem) — sleep for at least req->tv_sec * 1e9 + req->tv_nsec ns.
// rem is ignored (no partial-sleep resume after signal for now).
static uint64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr) {
    (void)rem_ptr;
    if (!req_ptr) return (uint64_t)-EINVAL;
    k_timespec_t* req = (k_timespec_t*)req_ptr;
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL) return (uint64_t)-EINVAL;

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
// Must match the struct timeval layout in userland/libc/libc.h exactly.
typedef struct { int64_t tv_sec; int64_t tv_usec; } k_timeval_t;

static uint64_t sys_gettimeofday(uint64_t tv_ptr, uint64_t tz_ptr) {
    (void)tz_ptr;
    if (!tv_ptr) return (uint64_t)-EINVAL;
    uint64_t ns = tsc_read_ns();
    k_timeval_t ktv;
    ktv.tv_sec  = (int64_t)(ns / 1000000000ULL);
    ktv.tv_usec = (int64_t)((ns % 1000000000ULL) / 1000ULL);
    return (copy_to_user((void*)tv_ptr, &ktv, sizeof(ktv)) == 0) ? 0 : (uint64_t)-EFAULT;
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

// ── sys_fb_map ────────────────────────────────────────────────────────────
// fb_map() → maps physical framebuffer into calling process's user space.
// Root only.  Returns virtual address or -errno.
static uint64_t sys_fb_map(void) {
    serial_puts_dbg("[fb_map] enter\n");
    if (!g_current) { serial_puts_dbg("[fb_map] no current\n"); return (uint64_t)-ESRCH; }
    if (g_current->cred.euid != 0) { serial_puts_dbg("[fb_map] not root\n"); return (uint64_t)-EPERM; }
    if (!g_fb.base_virt) { serial_puts_dbg("[fb_map] no fb\n"); return (uint64_t)-EIO; }

    uint64_t fb_phys = g_fb.base_virt - HHDM_OFFSET;
    uint64_t fb_size = (uint64_t)g_fb.pitch * g_fb.height;

    mm_t* mm = g_current->mm_shared->mm;
    if (!mm) return (uint64_t)-EINVAL;

    virt_addr_t vaddr = vmm_map_physical_user(
        mm, g_current->mm_shared->pml4_phys, fb_phys, fb_size);
    serial_puts_dbg("[fb_map] vaddr="); serial_hex_dbg(vaddr);
    if (!vaddr) return (uint64_t)-ENOMEM;

    // Compositor takes ownership — detach TTY0 console output
    extern tty_t g_tty0;
    g_tty0.write_char = NULL;

    return vaddr;
}

// ── sys_openpty ──────────────────────────────────────────────────────────
// openpty(int fds[2]) → 0 or -errno
// Allocates a PTY master/slave pair.  fds[0] = master, fds[1] = slave.
#include "pty.h"
static int64_t fd_install(vfs_file_t* f);  // forward decl

static uint64_t sys_openpty(uint64_t user_fds) {
    if (!g_current) return (uint64_t)-ESRCH;

    vfs_file_t* master = NULL;
    vfs_file_t* slave = NULL;
    int err = pty_alloc(&master, &slave);
    if (err < 0) return (uint64_t)(int64_t)err;

    // Install into fd table
    int64_t mfd = fd_install(master);
    if (mfd < 0) {
        master->close(master);
        slave->close(slave);
        return (uint64_t)mfd;
    }

    int64_t sfd = fd_install(slave);
    if (sfd < 0) {
        // Close master fd
        g_current->files_shared->fd_table[mfd] = NULL;
        master->close(master);
        slave->close(slave);
        return (uint64_t)sfd;
    }

    // Copy fds to userspace
    int fds[2] = { (int)mfd, (int)sfd };
    if (copy_to_user((void*)user_fds, fds, sizeof(fds)) != 0) {
        g_current->files_shared->fd_table[mfd] = NULL;
        g_current->files_shared->fd_table[sfd] = NULL;
        master->close(master);
        slave->close(slave);
        return (uint64_t)-EFAULT;
    }

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
    serial_puts_dbg("[socket] domain="); serial_hex_dbg(domain);
    vfs_file_t* f;

    if (domain == AF_UNIX) {
        f = unix_sock_open((int)type);
    } else if (domain == AF_INET) {
        if (!net_ready()) return (uint64_t)-ENETDOWN;
        f = socket_open((int)domain, (int)type);
    } else {
        return (uint64_t)-EAFNOSUPPORT;
    }

    if (!f) { serial_puts_dbg("[socket] ENOMEM\n"); return (uint64_t)-ENOMEM; }
    int64_t fd = fd_install(f);
    serial_puts_dbg("[socket] fd="); serial_hex_dbg((uint64_t)fd);
    return (fd < 0) ? (uint64_t)fd : (uint64_t)fd;
}

// Helper: is this vfs_file_t a unix socket?
static inline int is_unix_sock(vfs_file_t* f) {
    return f && f->close == unix_sock_close;
}

// bind(fd, sockaddr*, addrlen) → 0 or -errno
static uint64_t sys_bind(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen) {
    (void)addrlen;
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    if (!addr_ptr) return (uint64_t)-EINVAL;

    if (is_unix_sock(f)) {
        const sockaddr_un_t* sa = (const sockaddr_un_t*)addr_ptr;
        if (sa->sun_family != AF_UNIX) return (uint64_t)-EINVAL;
        serial_puts_dbg("[bind] unix path="); serial_puts_dbg(sa->sun_path); serial_putc_dbg('\n');
        int r = unix_sock_bind(f, sa->sun_path);
        serial_puts_dbg("[bind] result="); serial_hex_dbg((uint64_t)(int64_t)r);
        return (uint64_t)(int64_t)r;
    }

    const sockaddr_in_t* sa = (const sockaddr_in_t*)addr_ptr;
    if (!sa) return (uint64_t)-EINVAL;
    uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));
    int r = socket_bind(f, port);
    return (uint64_t)(int64_t)r;
}

// listen(fd, backlog) → 0 or -errno
static uint64_t sys_listen(uint64_t fd, uint64_t backlog) {
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;

    if (is_unix_sock(f)) {
        serial_puts_dbg("[listen] unix backlog="); serial_hex_dbg(backlog);
        int r = unix_sock_listen(f, (int)backlog);
        serial_puts_dbg("[listen] result="); serial_hex_dbg((uint64_t)(int64_t)r);
        return (uint64_t)(int64_t)r;
    }

    (void)backlog;
    int r = socket_listen(f);
    return (uint64_t)(int64_t)r;
}

// accept(fd, sockaddr*, addrlen*) → new fd or -errno
static uint64_t sys_accept(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    (void)addrlen_ptr;
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;

    if (is_unix_sock(f)) {
        serial_puts_dbg("[accept] unix waiting...\n");
        vfs_file_t* cf = unix_sock_accept(f);
        if (!cf) { serial_puts_dbg("[accept] failed\n"); return (uint64_t)-ECONNABORTED; }
        int64_t nfd = fd_install(cf);
        serial_puts_dbg("[accept] new fd="); serial_hex_dbg((uint64_t)nfd);
        return (nfd < 0) ? (uint64_t)nfd : (uint64_t)nfd;
    }

    sockaddr_in_t* peer = (sockaddr_in_t*)addr_ptr;
    vfs_file_t* cf = socket_accept(f, peer);
    if (!cf) {
        // Non-blocking accept: no ready child. Otherwise a real failure.
        if (f->flags & O_NONBLOCK) return (uint64_t)-EAGAIN;
        return (uint64_t)-ECONNABORTED;
    }
    if (peer) {
        // Convert port back to network byte order for userspace.
        peer->sin_port = (uint16_t)((peer->sin_port >> 8) | (peer->sin_port << 8));
    }
    int64_t nfd = fd_install(cf);
    return (nfd < 0) ? (uint64_t)nfd : (uint64_t)nfd;
}

// connect(fd, sockaddr*, addrlen) → 0 or -errno
static uint64_t sys_connect(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen) {
    (void)addrlen;
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    if (!addr_ptr) return (uint64_t)-EINVAL;

    if (is_unix_sock(f)) {
        const sockaddr_un_t* sa = (const sockaddr_un_t*)addr_ptr;
        if (sa->sun_family != AF_UNIX) return (uint64_t)-EINVAL;
        serial_puts_dbg("[connect] unix path="); serial_puts_dbg(sa->sun_path); serial_putc_dbg('\n');
        int r = unix_sock_connect(f, sa->sun_path);
        serial_puts_dbg("[connect] result="); serial_hex_dbg((uint64_t)(int64_t)r);
        return (uint64_t)(int64_t)r;
    }

    const sockaddr_in_t* sa = (const sockaddr_in_t*)addr_ptr;
    uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));
    int r = socket_connect(f, sa->sin_addr, port);
    return (uint64_t)(int64_t)r;
}

// sendto(fd, buf, len, flags, addr, addrlen) → bytes or -errno
static uint64_t sys_sendto(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                             uint64_t flags, uint64_t addr_ptr, uint64_t addrlen) {
    (void)flags; (void)addrlen;
    vfs_file_t* f = fd_to_file(fd);
    if (!f || !buf_ptr || !len) return (uint64_t)-EINVAL;

    if (is_unix_sock(f)) {
        if (addr_ptr) {
            const sockaddr_un_t* sa = (const sockaddr_un_t*)addr_ptr;
            int r = unix_sock_sendto(f, (const void*)buf_ptr, (uint32_t)len,
                                      sa->sun_path);
            return (r < 0) ? (uint64_t)(int64_t)r : (uint64_t)r;
        }
        int r = unix_sock_send(f, (const void*)buf_ptr, (uint32_t)len);
        return (r < 0) ? (uint64_t)(int64_t)r : (uint64_t)r;
    }

    const sockaddr_in_t* sa = (const sockaddr_in_t*)addr_ptr;
    int r;
    if (sa) {
        uint16_t port = (uint16_t)((sa->sin_port >> 8) | (sa->sin_port << 8));
        r = socket_sendto(f, (const void*)buf_ptr, (uint32_t)len,
                           sa->sin_addr, port);
    } else {
        r = socket_send(f, (const void*)buf_ptr, (uint32_t)len);
    }
    return (uint64_t)(int64_t)r;
}

// recvfrom(fd, buf, len, flags, addr, addrlen*) → bytes or -errno
static uint64_t sys_recvfrom(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                               uint64_t flags, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    (void)flags; (void)addrlen_ptr;
    vfs_file_t* f = fd_to_file(fd);
    if (!f || !buf_ptr || !len) return (uint64_t)-EINVAL;

    if (is_unix_sock(f)) {
        int r = unix_sock_recv(f, (void*)buf_ptr, (uint32_t)len);
        return (r < 0) ? (uint64_t)(int64_t)r : (uint64_t)r;
    }

    sockaddr_in_t* sa = (sockaddr_in_t*)addr_ptr;
    int r;
    if (sa) {
        r = socket_recvfrom(f, (void*)buf_ptr, (uint32_t)len, sa);
        // src_port is already in network byte order (stamped from skb).
    } else {
        r = socket_recv(f, (void*)buf_ptr, (uint32_t)len);
    }
    return (uint64_t)(int64_t)r;
}

// setsockopt(fd, level, optname, optval*, optlen) → 0 or -errno
static uint64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t opt,
                                uint64_t val_ptr, uint64_t vallen) {
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    if (is_unix_sock(f)) {
        // Unix sockets: accept everything silently (no options honoured yet).
        return 0;
    }
    int r = socket_setsockopt(f, (int)level, (int)opt,
                               (const void*)val_ptr, (uint32_t)vallen);
    return (uint64_t)(int64_t)r;
}

// getpeerpid(fd) → pid of peer on AF_UNIX SOCK_STREAM conn, or -errno.
// Kernel-trusted — stamped at accept()/connect(). The compositor uses this
// to SIGKILL an unresponsive client after the user force-closes its window.
static uint64_t sys_getpeerpid(uint64_t fd) {
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;
    if (!is_unix_sock(f)) return (uint64_t)-ENOTSOCK;
    unix_sock_t* s = (unix_sock_t*)f->ctx;
    if (!s) return (uint64_t)-EBADF;
    if (s->state != UNIX_STATE_CONNECTED) return (uint64_t)-ENOTCONN;
    if (s->peer_pid == 0) return (uint64_t)-ENOTCONN;
    return (uint64_t)s->peer_pid;
}

// net_ifconfig(ifcfg_t*, len) → 0 or -errno
// Root-only syscall used by dhcpcd to configure the primary interface
// after a successful DHCP lease.  Writes IP/gw/mask and the DNS server
// list into the kernel's network state so resolvers and routers see the
// new values immediately.
static uint64_t sys_net_ifconfig(uint64_t cfg_ptr, uint64_t len) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (g_current->cred.euid != 0) return (uint64_t)-EPERM;
    if (!cfg_ptr || len < sizeof(ifcfg_t)) return (uint64_t)-EINVAL;

    ifcfg_t cfg;
    if (copy_from_user(&cfg, (const void*)cfg_ptr, sizeof(cfg)) != 0)
        return (uint64_t)-EFAULT;

    net_set_config(cfg.ip_be, cfg.gateway_be, cfg.netmask_be);

    uint32_t n = 0;
    uint32_t dns[IFCFG_MAX_DNS];
    for (uint32_t i = 0; i < IFCFG_MAX_DNS; i++) {
        if (cfg.dns_be[i] != 0) dns[n++] = cfg.dns_be[i];
    }
    net_set_dns(dns, n);
    return 0;
}

// net_mac(uint8_t out[6]) → 0 or -errno
// Returns the primary NIC's hardware address.  Used by dhcpcd to build
// DHCP packets (chaddr field) and by any userland tool that needs to
// display the interface MAC.  Not root-restricted — the MAC is broadcast
// on every frame we send, so exposing it is not a privacy leak.
static uint64_t sys_net_mac(uint64_t out_ptr) {
    if (!out_ptr) return (uint64_t)-EINVAL;
    const uint8_t* mac = virtio_net_mac();
    if (!mac) return (uint64_t)-EIO;
    if (copy_to_user((void*)out_ptr, mac, 6) != 0)
        return (uint64_t)-EFAULT;
    return 0;
}

// shutdown(fd, how) → 0 or -errno
static uint64_t sys_shutdown(uint64_t fd, uint64_t how) {
    vfs_file_t* f = fd_to_file(fd);
    if (!f) return (uint64_t)-EBADF;

    if (is_unix_sock(f))
        return (uint64_t)(int64_t)unix_sock_shutdown(f, (int)how);

    int r = socket_shutdown(f, (int)how);
    return (uint64_t)(int64_t)r;
}

static inline int _access_ok(uint64_t addr, uint64_t len) {
    if (!addr) return 0;
    if (addr >= HHDM_OFFSET) return 0;
    if (len && (addr + len) < addr) return 0;       // overflow
    if (len && (addr + len) > HHDM_OFFSET) return 0;
    return 1;
}

// ── Helper: copy bytes from user to kernel safely ─────────────────────────
// Returns 0 on success, -EFAULT if the pointer is bad or in kernel space.
static int copy_from_user(void* dst, const void* src_u, uint64_t len) {
    if (!_access_ok((uint64_t)src_u, len)) return -EFAULT;
    user_buf_prefault((virt_addr_t)src_u, len);
    __builtin_memcpy(dst, src_u, len);
    return 0;
}

static int copy_to_user(void* dst_u, const void* src, uint64_t len) {
    if (!_access_ok((uint64_t)dst_u, len)) return -EFAULT;
    user_buf_prefault((virt_addr_t)dst_u, len);
    __builtin_memcpy(dst_u, src, len);
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
// fstat(fd, struct stat*) → 0 or -errno
static uint64_t sys_fstat(uint64_t fd, uint64_t stat_ptr) {
    if (!g_current || !stat_ptr) return (uint64_t)-EINVAL;
    task_files_t* files = g_current->files_shared;
    if (fd >= files->fd_capacity || !files->fd_table[fd])
        return (uint64_t)-EBADF;

    vfs_file_t* f = files->fd_table[fd];
    struct stat kst;
    __builtin_memset(&kst, 0, sizeof(kst));

    // If the file has a stat path stored, look up ext2.
    if (f->path[0]) {
        ext2_inode_t inode;
        uint32_t inum = ext2_lookup_path_raw(f->path);
        if (inum && ext2_read_inode(inum, &inode)) {
            int is_dir = ((inode.i_mode & 0xF000) == 0x4000);
            kst.st_ino     = inum;
            kst.st_mode    = inode.i_mode;
            kst.st_size    = inode.i_size;
            kst.st_uid     = inode.i_uid;
            kst.st_gid     = inode.i_gid;
            kst.st_nlink   = is_dir ? 2 : 1;
            kst.st_blksize = 4096;
            kst.st_blocks  = (int64_t)((inode.i_size + 511) / 512);
        }
    } else {
        // Device/pipe/socket/tty: fill minimal stat (char device, rw).
        kst.st_mode    = 0020666; // S_IFCHR | 0666
        kst.st_nlink   = 1;
        kst.st_blksize = 4096;
    }

    return (copy_to_user((void*)stat_ptr, &kst, sizeof(kst)) == 0) ? 0 : (uint64_t)-EFAULT;
}

// ── sys_access ────────────────────────────────────────────────────────────
// access(path, mode) → 0 or -errno
// Checks real UID/GID permissions (F_OK, R_OK, W_OK, X_OK).
static uint64_t sys_access(uint64_t path_ptr, uint64_t amode) {
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

    // Unified lookup for both virtfs and ext2.
    {
        uint8_t ac_need = 0;
        if (amode & 4) ac_need |= ACL_PERM_READ;
        if (amode & 2) ac_need |= ACL_PERM_WRITE;
        if (amode & 1) ac_need |= ACL_PERM_EXEC;
        // F_OK (amode==0): existence check only — no permission needed on the file itself.
        // The path walk in ext2_lookup_path checks exec on parent dirs, which is correct.
        fs_node_t fsn;
        int fsr = fs_lookup(path, &g_current->cred, ac_need, &fsn);
        if (fsr != 0) return (uint64_t)(int64_t)fsr;
        if (fsn.is_virtual) return 0; // already fully checked
    }

    // ext2: fs_lookup already checked existence and permissions.
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

// ── Identity syscalls ─────────────────────────────────────────────────────
static uint64_t sys_getuid(void)  { return g_current ? g_current->cred.ruid : 0; }
static uint64_t sys_geteuid(void) { return g_current ? g_current->cred.euid : 0; }
static uint64_t sys_getgid(void)  { return g_current ? g_current->cred.rgid : 0; }
static uint64_t sys_getegid(void) { return g_current ? g_current->cred.egid : 0; }

static uint64_t sys_getgroups(uint64_t size, uint64_t list_ptr) {
    if (!g_current) return (uint64_t)-EINVAL;
    if ((int64_t)size < 0) return (uint64_t)-EINVAL;
    uint8_t n = g_current->cred.ngroups;
    if (size == 0) return (uint64_t)n;
    if (size < (uint64_t)n) return (uint64_t)-EINVAL;
    if (copy_to_user((void*)list_ptr, g_current->cred.supplemental,
                     n * sizeof(uint32_t)) != 0)
        return (uint64_t)-EFAULT;
    return (uint64_t)n;
}

// ── sys_setuid / seteuid / setgid / setegid / setreuid / setregid ─────────
//
// In-slot transitions (within the three POSIX slots) are handled directly.
// Genuine escalation (new uid not in {ruid, euid, suid}) → ask ksec.

static uint64_t sys_setuid(uint64_t uid) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (cred_setuid(&g_current->cred, (uint32_t)uid) == 0) return 0;
    // Escalation path: ask ksec.
    if (!ksec_agent_present()) return (uint64_t)-EPERM;
    ksec_request_t req = {0};
    ksec_response_t resp = {0};
    req.seq        = ksec_next_seq();
    req.op         = KSEC_OP_SETUID;
    req.caller_pid = g_current->pid;
    req.caller_uid = g_current->cred.euid;
    req.caller_gid = g_current->cred.egid;
    req.target_uid = (uint32_t)uid;
    if (ksec_request(&req, &resp) != 0) return (uint64_t)-EPERM;
    if (resp.verdict != KSEC_VERDICT_ALLOW) return (uint64_t)-EPERM;
    // Root-style: set all three.
    g_current->cred.ruid = g_current->cred.euid = g_current->cred.suid =
        resp.granted_euid;
    return 0;
}

static uint64_t sys_seteuid(uint64_t euid) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (cred_seteuid(&g_current->cred, (uint32_t)euid) == 0) return 0;
    if (!ksec_agent_present()) return (uint64_t)-EPERM;
    ksec_request_t req = {0};
    ksec_response_t resp = {0};
    req.seq        = ksec_next_seq();
    req.op         = KSEC_OP_SETEUID;
    req.caller_pid = g_current->pid;
    req.caller_uid = g_current->cred.euid;
    req.caller_gid = g_current->cred.egid;
    req.target_uid = (uint32_t)euid;
    if (ksec_request(&req, &resp) != 0) return (uint64_t)-EPERM;
    if (resp.verdict != KSEC_VERDICT_ALLOW) return (uint64_t)-EPERM;
    g_current->cred.euid = resp.granted_euid;
    return 0;
}

static uint64_t sys_setgid(uint64_t gid) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (cred_setgid(&g_current->cred, (uint32_t)gid) == 0) return 0;
    return (uint64_t)-EPERM;   // group escalation not via ksec currently
}

static uint64_t sys_setegid(uint64_t egid) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (cred_setegid(&g_current->cred, (uint32_t)egid) == 0) return 0;
    return (uint64_t)-EPERM;
}

static uint64_t sys_setreuid(uint64_t ruid, uint64_t euid) {
    if (!g_current) return (uint64_t)-EINVAL;
    cred_t* c = &g_current->cred;
    // POSIX: -1 means "leave unchanged".
    uint32_t new_ruid = (ruid == (uint64_t)-1) ? c->ruid : (uint32_t)ruid;
    uint32_t new_euid = (euid == (uint64_t)-1) ? c->euid : (uint32_t)euid;
    // Root may set any combination.
    if (!cred_is_root(c)) {
        // Non-root: new_ruid must be in {ruid, euid}, new_euid must be in {ruid, euid, suid}.
        int ruid_ok = (new_ruid == c->ruid || new_ruid == c->euid);
        int euid_ok = (new_euid == c->ruid || new_euid == c->euid || new_euid == c->suid);
        if (!ruid_ok || !euid_ok) return (uint64_t)-EPERM;
    }
    // If ruid changed or euid != old ruid, update suid.
    if (new_ruid != c->ruid || new_euid != c->ruid)
        c->suid = new_euid;
    c->ruid = new_ruid;
    c->euid = new_euid;
    return 0;
}

static uint64_t sys_setregid(uint64_t rgid, uint64_t egid) {
    if (!g_current) return (uint64_t)-EINVAL;
    cred_t* c = &g_current->cred;
    uint32_t new_rgid = (rgid == (uint64_t)-1) ? c->rgid : (uint32_t)rgid;
    uint32_t new_egid = (egid == (uint64_t)-1) ? c->egid : (uint32_t)egid;
    if (!cred_is_root(c)) {
        int rgid_ok = (new_rgid == c->rgid || new_rgid == c->egid);
        int egid_ok = (new_egid == c->rgid || new_egid == c->egid || new_egid == c->sgid);
        if (!rgid_ok || !egid_ok) return (uint64_t)-EPERM;
    }
    if (new_rgid != c->rgid || new_egid != c->rgid)
        c->sgid = new_egid;
    c->rgid = new_rgid;
    c->egid = new_egid;
    return 0;
}

static uint64_t sys_setgroups(uint64_t size, uint64_t list_ptr) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (!cred_is_root(&g_current->cred)) return (uint64_t)-EPERM;
    if (size > CRED_NGROUPS_MAX) return (uint64_t)-EINVAL;
    if (size > 0 && !list_ptr) return (uint64_t)-EFAULT;
    uint32_t buf[CRED_NGROUPS_MAX];
    if (size > 0 && copy_from_user(buf, (const void*)list_ptr,
                                    size * sizeof(uint32_t)) != 0)
        return (uint64_t)-EFAULT;
    uint8_t i;
    for (i = 0; i < (uint8_t)size; i++)
        g_current->cred.supplemental[i] = buf[i];
    g_current->cred.ngroups = (uint8_t)size;
    return 0;
}

// ── sys_pledge ────────────────────────────────────────────────────────────
// pledge(mask) → 0 or -errno
// Restricts the process to the given syscall groups.  AND-only — irreversible.
static uint64_t sys_pledge(uint64_t mask) {
    if (!g_current) return (uint64_t)-EINVAL;
    g_current->pledge_mask = pledge_restrict(g_current->pledge_mask, (uint32_t)mask);
    return 0;
}

// ── sys_unveil ────────────────────────────────────────────────────────────
// unveil(path_ptr, pathlen, perms) → 0 or -errno
static uint64_t sys_unveil(uint64_t path_ptr, uint64_t pathlen, uint64_t perms) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (pathlen == 0 || pathlen >= UNVEIL_PATH_MAX) return (uint64_t)-EINVAL;
    char path[UNVEIL_PATH_MAX];
    const char* upath = (const char*)path_ptr;
    uint64_t i;
    for (i = 0; i < pathlen; i++) path[i] = upath[i];
    path[pathlen] = '\0';
    int r = unveil_add(&g_current->unveil, path, (uint8_t)perms);
    if (r != 0) return (uint64_t)(int64_t)r;
    return 0;
}

// unveil_lock() → 0
static uint64_t sys_unveil_lock(void) {
    if (!g_current) return (uint64_t)-EINVAL;
    unveil_lock(&g_current->unveil);
    return 0;
}

// ── sys_restrict_fd ───────────────────────────────────────────────────────
// restrict_fd(fd, rights_mask) → 0 or -errno
// ANDs the fd's rights bitmask with `rights_mask`.  Irrevocable downgrade.
static uint64_t sys_restrict_fd(uint64_t fd, uint64_t rights_mask) {
    if (!g_current) return (uint64_t)-EBADF;
    task_files_t* files = g_current->files_shared;
    if (fd >= files->fd_capacity || !files->fd_table[fd])
        return (uint64_t)-EBADF;
    files->fd_table[fd]->rights = rights_restrict(files->fd_table[fd]->rights,
                                                    (uint32_t)rights_mask);
    return 0;
}

// ── sys_sendfd ────────────────────────────────────────────────────────────
// sendfd(sock_fd, target_fd, new_rights) → 0 or -errno
// Transfers target_fd over the Unix socket sock_fd via SCM_RIGHTS.
// new_rights must be a subset of target_fd's current rights (attenuation).
// The receiving side calls sys_recvfd to get a new fd stamped with new_rights.
static uint64_t sys_sendfd(uint64_t sock_fd, uint64_t target_fd_num,
                            uint64_t new_rights) {
    serial_puts_dbg("[sendfd] sock="); serial_hex_dbg(sock_fd);
    if (!g_current) return (uint64_t)-EBADF;
    task_files_t* files = g_current->files_shared;

    // Validate sock_fd — must be a connected unix socket.
    if (sock_fd >= files->fd_capacity || !files->fd_table[sock_fd])
        return (uint64_t)-EBADF;
    vfs_file_t* sock = files->fd_table[sock_fd];
    if (!is_unix_sock(sock)) return (uint64_t)-ENOTSOCK;
    if (!rights_check(sock->rights, RIGHT_SEND_FD))
        return (uint64_t)-EPERM;

    // Validate target_fd.
    if (target_fd_num >= files->fd_capacity || !files->fd_table[target_fd_num])
        return (uint64_t)-EBADF;
    vfs_file_t* target_f = files->fd_table[target_fd_num];

    // new_rights must be a subset of target_fd's current rights.
    if (((uint32_t)new_rights & ~target_f->rights) != 0)
        return (uint64_t)-EPERM;

    int r = unix_sock_sendfd(sock, target_f, (uint32_t)new_rights);
    serial_puts_dbg("[sendfd] r="); serial_hex_dbg((uint64_t)(int64_t)r);
    return (r < 0) ? (uint64_t)(int64_t)r : 0;
}

// ── sys_recvfd ───────────────────────────────────────────────────────────
// recvfd(sock_fd) → new fd or -errno
// Dequeues a file descriptor sent via sendfd() over a Unix socket.
static uint64_t sys_recvfd(uint64_t sock_fd) {
    serial_puts_dbg("[recvfd] sock="); serial_hex_dbg(sock_fd);
    if (!g_current) return (uint64_t)-EBADF;
    task_files_t* files = g_current->files_shared;

    if (sock_fd >= files->fd_capacity || !files->fd_table[sock_fd])
        return (uint64_t)-EBADF;
    vfs_file_t* sock = files->fd_table[sock_fd];
    if (!is_unix_sock(sock)) return (uint64_t)-ENOTSOCK;

    vfs_file_t* received = unix_sock_recvfd(sock);
    if (!received) { serial_puts_dbg("[recvfd] NULL\n"); return (uint64_t)-EAGAIN; }

    int64_t fd = fd_install(received);
    serial_puts_dbg("[recvfd] fd="); serial_hex_dbg((uint64_t)fd);
    return (fd < 0) ? (uint64_t)fd : (uint64_t)fd;
}

// ── sys_register_policy_agent ─────────────────────────────────────────────
// register_policy_agent(read_fd, write_fd) → 0 or -errno
// Boot-only, uid=0 only.  Registers the ksec daemon as the policy agent.
static uint64_t sys_register_policy_agent(uint64_t read_fd, uint64_t write_fd) {
    if (!g_current) return (uint64_t)-EINVAL;

    // Only uid=0 processes may register.
    if (g_current->cred.euid != 0) return (uint64_t)-EPERM;

    // Only allowed if no agent is registered yet (enforced inside ksec_register_agent).
    task_files_t* files = g_current->files_shared;
    if (read_fd  >= files->fd_capacity || !files->fd_table[read_fd])
        return (uint64_t)-EBADF;
    if (write_fd >= files->fd_capacity || !files->fd_table[write_fd])
        return (uint64_t)-EBADF;

    vfs_file_t* rp = vfs_dup(files->fd_table[read_fd]);
    vfs_file_t* wp = vfs_dup(files->fd_table[write_fd]);

    int r = ksec_register_agent(rp, wp);
    if (r != 0) {
        vfs_close(rp);
        vfs_close(wp);
        return (uint64_t)(int64_t)r;
    }

    // Start the ksec reader kernel thread.
    task_t* reader = task_create_kthread(ksec_reader_thread, pid_alloc());
    if (reader) sched_add(reader);

    return 0;
}

// ── Process group / session syscalls ─────────────────────────────────────

// Find a task by pid. Returns NULL if not found.
// O(1) via sched's pid hash table.
extern task_t* sched_find_pid(uint32_t pid);

static uint64_t sys_setpgid(uint64_t pid_arg, uint64_t pgid_arg) {
    uint32_t pid  = pid_arg  ? (uint32_t)pid_arg  : g_current->pid;
    uint32_t pgid = pgid_arg ? (uint32_t)pgid_arg : pid;

    task_t* t = (pid == g_current->pid) ? g_current : sched_find_pid(pid);
    if (!t) return (uint64_t)-ESRCH;

    // Can't change pgid of a session leader.
    if (t->pid == t->sid) return (uint64_t)-EPERM;

    // Move between pgid hash buckets atomically w.r.t. signal delivery.
    task_idx_pgid_changing(t);
    t->pgid = pgid;
    task_idx_pgid_changed(t);
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

    task_idx_sid_changing(g_current);
    task_idx_pgid_changing(g_current);
    g_current->sid  = g_current->pid;
    g_current->pgid = g_current->pid;
    task_idx_pgid_changed(g_current);
    task_idx_sid_changed(g_current);

    // New session leader starts without a controlling terminal.
    // The tty fg_pgid is updated when the shell calls TIOCSCTTY/TIOCSPGRP.
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
    (void)fd;
    tty_t* tty = tty_get_ctty();
    if (!tty) tty = &g_tty0;
    return (uint64_t)tty->fg_pgid;
}

static uint64_t sys_tcsetpgrp(uint64_t fd, uint64_t pgid) {
    (void)fd;
    tty_t* tty = tty_get_ctty();
    if (!tty) tty = &g_tty0;
    tty->fg_pgid = (uint32_t)pgid;
    return 0;
}

// ── sys_ioctl ─────────────────────────────────────────────────────────────
// ioctl(fd, request, arg) → varies
static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
    if (!g_current) return (uint64_t)-EBADF;

    // Validate fd
    if (fd < g_current->files_shared->fd_capacity &&
        !g_current->files_shared->fd_table[fd] && fd > 2)
        return (uint64_t)-EBADF;

    // If the file has its own ioctl handler (e.g. PTY slave), use it.
    vfs_file_t* f = NULL;
    if (fd < g_current->files_shared->fd_capacity)
        f = g_current->files_shared->fd_table[fd];
    if (f && f->ioctl)
        return (uint64_t)f->ioctl(f, request, arg);

    // Fallback: resolve to controlling tty (tty0 for console processes).
    tty_t* tty = &g_tty0;

    switch (request) {
    case TIOCGWINSZ:
        return (copy_to_user((void*)arg, &tty->winsize, sizeof(tty->winsize)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TIOCSWINSZ: {
        winsize_t ws;
        if (copy_from_user(&ws, (const void*)arg, sizeof(ws)) != 0)
            return (uint64_t)-EFAULT;
        tty->winsize = ws;
        // Notify foreground pgrp of window size change.
        if (tty->fg_pgid) signal_send_pgrp(tty->fg_pgid, SIGWINCH);
        return 0;
    }
    case TIOCGPGRP:
        return (copy_to_user((void*)arg, &tty->fg_pgid, sizeof(tty->fg_pgid)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TIOCSPGRP: {
        uint32_t pg = 0;
        if (copy_from_user(&pg, (const void*)arg, sizeof(pg)) != 0)
            return (uint64_t)-EFAULT;
        tty->fg_pgid = pg;
        return 0;
    }
    case TCGETS:
        return (copy_to_user((void*)arg, &tty->termios, sizeof(tty->termios)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TCSETS:
    case TCSETSW:
        return (copy_from_user(&tty->termios, (const void*)arg, sizeof(tty->termios)) == 0)
               ? 0 : (uint64_t)-EFAULT;
    case TCSETSF: {
        int r = copy_from_user(&tty->termios, (const void*)arg, sizeof(tty->termios));
        tty_flush_input(tty);
        return (r == 0) ? 0 : (uint64_t)-EFAULT;
    }
    case TCSBRK:
    case TCXONC:
    case TCFLSH:
        tty_flush_input(tty);
        return 0;
    case TIOCEXCL:
    case TIOCNXCL:
        return 0;  // Acknowledged, no-op.
    case TIOCSCTTY:
        tty_set_ctty(tty);
        return 0;
    case TIOCGSERIAL:
        if (arg) {
            uint8_t* p = (uint8_t*)arg;
            __builtin_memset(p, 0, 60);
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

static int fd_has_hup(vfs_file_t* f) {
    if (!f || !f->poll) return 0;
    return f->poll(f, POLLHUP);
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
    int sel_infinite = 1;
    uint64_t timeout_ns = 0;
    if (tv_ptr) {
        k_timeval_t tv;
        copy_from_user(&tv, (const void*)tv_ptr, sizeof(tv));
        timeout_ns = (uint64_t)tv.tv_sec * 1000000000ULL
                   + (uint64_t)tv.tv_usec * 1000ULL;
        sel_infinite = 0;
    }

    extern uint64_t tsc_read_ns(void);
    uint64_t deadline = sel_infinite ? UINT64_MAX : (tsc_read_ns() + timeout_ns);
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
        if (!sel_infinite && timeout_ns == 0) break;
        // Register one task_we_t per watched fd (on both queues if secondary exists).
        // 2*nfds slots worst-case; unused slots stay zeroed.
        uint32_t sel_nslots = nfds * 2;
        task_we_t* sel_wes = (task_we_t*)kmalloc(sel_nslots * sizeof(task_we_t));
        uint32_t sel_used = 0;
        if (sel_wes) {
            __builtin_memset(sel_wes, 0, sel_nslots * sizeof(task_we_t));
            for (uint32_t fd = 0; fd < nfds; fd++) {
                if (!FD_ISSET(fd, &rset) && !FD_ISSET(fd, &wset) &&
                    !FD_ISSET(fd, &eset)) continue;
                vfs_file_t* f = (fd < files->fd_capacity) ? files->fd_table[fd] : NULL;
                if (!f) continue;
                if (sel_used < sel_nslots) {
                    task_we_init(&sel_wes[sel_used], g_current);
                    task_we_add(f->waitq, &sel_wes[sel_used]);
                    sel_used++;
                }
                if (f->secondary_waitq && sel_used < sel_nslots) {
                    task_we_init(&sel_wes[sel_used], g_current);
                    task_we_add(f->secondary_waitq, &sel_wes[sel_used]);
                    sel_used++;
                }
            }
        }
        // Re-check after registering — close race between first check and add.
        int sel_recheck = 0;
        for (uint32_t fd = 0; fd < nfds && !sel_recheck; fd++) {
            vfs_file_t* f = (fd < files->fd_capacity) ? files->fd_table[fd] : NULL;
            if (!f) continue;
            if ((FD_ISSET(fd, &rset) && fd_is_readable(f)) ||
                (FD_ISSET(fd, &wset) && fd_is_writable(f))) sel_recheck = 1;
        }
        if (!sel_recheck) {
            g_current->sleep_until_ns = sel_infinite ? 0 : deadline;
            sched_sleep();
            g_current->sleep_until_ns = 0;
        }
        // Remove all entries still on a queue (wq_remove is a no-op if already gone).
        if (sel_wes) {
            uint32_t si = 0;
            for (uint32_t fd = 0; fd < nfds && si < sel_used; fd++) {
                if (!FD_ISSET(fd, &rset) && !FD_ISSET(fd, &wset) &&
                    !FD_ISSET(fd, &eset)) continue;
                vfs_file_t* f = (fd < files->fd_capacity) ? files->fd_table[fd] : NULL;
                if (!f) continue;
                if (si < sel_used) { task_we_remove(f->waitq, &sel_wes[si]); si++; }
                if (f->secondary_waitq && si < sel_used) {
                    task_we_remove(f->secondary_waitq, &sel_wes[si]); si++;
                }
            }
            kfree(sel_wes);
        }
    } while (sel_infinite || tsc_read_ns() < deadline);

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

    int infinite = (timeout_ms == (uint64_t)-1);
    uint64_t timeout_ns = infinite ? 0 : (timeout_ms * 1000000ULL);
    extern uint64_t tsc_read_ns(void);
    uint64_t deadline = infinite ? UINT64_MAX : (tsc_read_ns() + timeout_ns);
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
            // POLLHUP and POLLERR are always reported, regardless of the
            // caller's events mask (POSIX requirement).
            if (fd_has_hup(f)) rev |= POLLHUP;
            if (rev) { ufds[i].revents = rev; count++; }
        }
        if (count > 0) break;
        if (timeout_ms == 0) break;
        // Register one task_we_t per watched fd (on both queues if secondary exists).
        uint32_t p_nslots = (uint32_t)nfds * 2;
        task_we_t* p_wes = (task_we_t*)kmalloc(p_nslots * sizeof(task_we_t));
        uint32_t p_used = 0;
        if (p_wes) {
            __builtin_memset(p_wes, 0, p_nslots * sizeof(task_we_t));
            for (uint64_t i = 0; i < nfds; i++) {
                int fd = ufds[i].fd;
                if (fd < 0) continue;
                vfs_file_t* f = ((uint32_t)fd < files->fd_capacity)
                                ? files->fd_table[fd] : NULL;
                if (!f) continue;
                if (p_used < p_nslots) {
                    task_we_init(&p_wes[p_used], g_current);
                    task_we_add(f->waitq, &p_wes[p_used]);
                    p_used++;
                }
                if (f->secondary_waitq && p_used < p_nslots) {
                    task_we_init(&p_wes[p_used], g_current);
                    task_we_add(f->secondary_waitq, &p_wes[p_used]);
                    p_used++;
                }
            }
        }
        // Re-check after registering — close race between first check and add.
        int recheck = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            int fd = ufds[i].fd;
            if (fd < 0) continue;
            vfs_file_t* f = ((uint32_t)fd < files->fd_capacity) ? files->fd_table[fd] : NULL;
            if (!f) continue;
            if (((ufds[i].events & POLLIN)  && fd_is_readable(f)) ||
                ((ufds[i].events & POLLOUT) && fd_is_writable(f)) ||
                fd_has_hup(f)) { recheck = 1; break; }
        }
        if (!recheck) {
            g_current->sleep_until_ns = infinite ? 0 : deadline;
            sched_sleep();
            g_current->sleep_until_ns = 0;
        }
        // Remove all entries still on a queue.
        if (p_wes) {
            uint32_t pi = 0;
            for (uint64_t i = 0; i < nfds && pi < p_used; i++) {
                int fd = ufds[i].fd;
                if (fd < 0) continue;
                vfs_file_t* f = ((uint32_t)fd < files->fd_capacity)
                                ? files->fd_table[fd] : NULL;
                if (!f) continue;
                if (pi < p_used) { task_we_remove(f->waitq, &p_wes[pi]); pi++; }
                if (f->secondary_waitq && pi < p_used) {
                    task_we_remove(f->secondary_waitq, &p_wes[pi]); pi++;
                }
            }
            kfree(p_wes);
        }
    } while (infinite || tsc_read_ns() < deadline);

    return (uint64_t)(int64_t)count;
}

// ── epoll ─────────────────────────────────────────────────────────────────
//
// Linux-accurate epoll using wait.h callback design (see kernel/include/wait.h):
//
//   epoll_ctl(ADD): kmallocs an epoll_watch_t, inserts it into an open-addressing
//     hash table keyed by fd.  Registers persistent epoll_we_t entries on the
//     fd's waitq (and secondary_waitq if present).  epoll_we_t.func =
//     epoll_wake_func: sets state->has_ready=1 and wakes state->wq.  Returns
//     WQ_KEEP — stays in the file's waitq until DEL/close.
//
//   epoll_wait(): registers ONE stack-allocated task_we_t on state->wq and
//     sleeps.  When any watched fd fires: fd.waitq → epoll_wake_func →
//     state->wq → task_we_t wakes the task (WQ_REMOVE).
//     Zero per-wakeup allocations after ADD.
//
//   epoll_ctl(DEL/MOD): O(1) hash lookup, unlinks entries, frees watch.
//
//   epoll_close(): drains all watch slots and frees state.
//
// Hash table: open addressing, power-of-2 capacity, load factor ≤ 75%.
// Initial cap = 16.  Grow-only (no shrink).  Tombstones for DEL.
// Memory per epoll fd: sizeof(epoll_state_t) + cap*8 bytes for the slot array
// + sizeof(epoll_watch_t) per watched fd.  No 40KB fixed allocation.

// Sentinel: slot contains EPOLL_DELETED after a DEL (tombstone).
#define EPOLL_DELETED ((epoll_watch_t*)1uL)
#define EPOLL_HT_INIT_CAP 16u

typedef struct {
    int32_t      fd;
    uint32_t     events;    // EPOLLIN | EPOLLOUT | ...
    epoll_data_t data;
    // Persistent epoll_we_t entries registered on the watched fd's queues.
    epoll_we_t   entry;     // on f->waitq
    epoll_we_t   entry2;    // on f->secondary_waitq (if any)
    int          has_entry2;
} epoll_watch_t;

typedef struct {
    epoll_watch_t** slots;  // open-addressing hash table (NULL=empty, DELETED=tomb)
    uint32_t        cap;    // number of slots (power of 2)
    uint32_t        count;  // live watches
    wait_queue_t    wq;     // sleeping tasks
    int             has_ready;
} epoll_state_t;

// Hash table helpers.  Key = fd (int32_t, always ≥ 0).
static uint32_t ep_hash(int32_t fd, uint32_t cap) {
    return (uint32_t)((uint32_t)fd * 2654435761u) & (cap - 1u);
}

// Find the slot index for `fd`.  Returns cap if not found.
static uint32_t ep_find(epoll_state_t* st, int32_t fd) {
    uint32_t i = ep_hash(fd, st->cap);
    for (uint32_t n = 0; n < st->cap; n++) {
        epoll_watch_t* s = st->slots[i];
        if (!s) return st->cap;                     // empty — not found
        if (s != EPOLL_DELETED && s->fd == fd) return i;
        i = (i + 1u) & (st->cap - 1u);
    }
    return st->cap;
}

// Insert w into the hash table (caller ensures cap > count and fd not present).
static void ep_ht_insert_raw(epoll_watch_t** slots, uint32_t cap, epoll_watch_t* w) {
    uint32_t i = ep_hash(w->fd, cap);
    for (;;) {
        epoll_watch_t* s = slots[i];
        if (!s || s == EPOLL_DELETED) { slots[i] = w; return; }
        i = (i + 1u) & (cap - 1u);
    }
}

// Grow the hash table to new_cap (power of 2, > cap).
// Returns 0 on success, -ENOMEM on failure (old table unchanged).
static int ep_grow(epoll_state_t* st, uint32_t new_cap) {
    epoll_watch_t** new_slots = (epoll_watch_t**)kmalloc(
        (uint64_t)new_cap * sizeof(epoll_watch_t*));
    if (!new_slots) return -ENOMEM;
    __builtin_memset(new_slots, 0, (uint64_t)new_cap * sizeof(epoll_watch_t*));
    // Re-insert all live entries.
    for (uint32_t i = 0; i < st->cap; i++) {
        epoll_watch_t* s = st->slots[i];
        if (s && s != EPOLL_DELETED) ep_ht_insert_raw(new_slots, new_cap, s);
    }
    kfree(st->slots);
    st->slots = new_slots;
    st->cap   = new_cap;
    return 0;
}

// Register/unregister persistent epoll_we_t entries on the watched fd's queues.
static void epoll_watch_register(epoll_state_t* state, epoll_watch_t* w,
                                  vfs_file_t* f) {
    epoll_we_init(&w->entry, &state->wq, &state->has_ready);
    epoll_we_add(f->waitq, &w->entry);
    if (f->secondary_waitq) {
        epoll_we_init(&w->entry2, &state->wq, &state->has_ready);
        epoll_we_add(f->secondary_waitq, &w->entry2);
        w->has_entry2 = 1;
    } else {
        w->has_entry2 = 0;
    }
}

static void epoll_watch_unregister(epoll_watch_t* w, vfs_file_t* f) {
    if (f) {
        epoll_we_remove(f->waitq, &w->entry);
        if (w->has_entry2 && f->secondary_waitq)
            epoll_we_remove(f->secondary_waitq, &w->entry2);
    }
    w->has_entry2 = 0;
}

// VFS ops for the epoll fd itself.
static int64_t epoll_read (vfs_file_t* self, void* buf, uint64_t len) {
    (void)self; (void)buf; (void)len; return (int64_t)-EINVAL;
}
static int64_t epoll_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self; (void)buf; (void)len; return (int64_t)-EINVAL;
}
static int64_t epoll_seek(vfs_file_t* self, int64_t off, int w) {
    (void)self; (void)off; (void)w; return (int64_t)-ESPIPE;
}
static void epoll_close(vfs_file_t* self) {
    if (self->ctx) {
        epoll_state_t* state = (epoll_state_t*)self->ctx;
        task_files_t* files = g_current ? g_current->files_shared : NULL;
        for (uint32_t i = 0; i < state->cap; i++) {
            epoll_watch_t* w = state->slots[i];
            if (!w || w == EPOLL_DELETED) continue;
            vfs_file_t* f = (files && (uint32_t)w->fd < files->fd_capacity)
                            ? files->fd_table[w->fd] : NULL;
            epoll_watch_unregister(w, f);
            kfree(w);
        }
        kfree(state->slots);
        kfree(state);
    }
    kfree(self);
}

// epoll_create1(flags) → fd or -errno
static uint64_t sys_epoll_create(uint64_t flags) {
    (void)flags;
    if (!g_current) return (uint64_t)-EINVAL;

    epoll_state_t* state = (epoll_state_t*)kmalloc(sizeof(epoll_state_t));
    if (!state) return (uint64_t)-ENOMEM;

    state->slots = (epoll_watch_t**)kmalloc(
        (uint64_t)EPOLL_HT_INIT_CAP * sizeof(epoll_watch_t*));
    if (!state->slots) { kfree(state); return (uint64_t)-ENOMEM; }
    __builtin_memset(state->slots, 0,
                      (uint64_t)EPOLL_HT_INIT_CAP * sizeof(epoll_watch_t*));
    state->cap       = EPOLL_HT_INIT_CAP;
    state->count     = 0;
    state->has_ready = 0;
    wait_queue_init(&state->wq);

    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(state->slots); kfree(state); return (uint64_t)-ENOMEM; }

    f->read           = epoll_read;
    f->write          = epoll_write;
    f->close          = epoll_close;
    f->seek           = epoll_seek;
    f->poll           = NULL;
    f->ioctl          = NULL;
    f->ctx            = state;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags          = 0;
    f->refcount       = 1;
    f->rights         = 0;
    f->path[0]        = '\0';

    int64_t efd = fd_install(f);
    if (efd < 0) { epoll_close(f); return (uint64_t)(int64_t)efd; }
    return (uint64_t)efd;
}

// epoll_ctl(epfd, op, fd, event*) → 0 or -errno
static uint64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd,
                               uint64_t event_ptr) {
    if (!g_current) return (uint64_t)-EINVAL;

    vfs_file_t* ef = fd_to_file(epfd);
    if (!ef || ef->close != epoll_close) return (uint64_t)-EBADF;
    epoll_state_t* state = (epoll_state_t*)ef->ctx;

    epoll_event_t ev;
    if (op != EPOLL_CTL_DEL) {
        if (!event_ptr) return (uint64_t)-EINVAL;
        if (copy_from_user(&ev, (const void*)event_ptr, sizeof(ev)) != 0)
            return (uint64_t)-EFAULT;
    }

    int32_t tfd = (int32_t)fd;
    task_files_t* files = g_current->files_shared;

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (ep_find(state, tfd) < state->cap) return (uint64_t)-EEXIST;
        vfs_file_t* wf = ((uint32_t)tfd < files->fd_capacity)
                         ? files->fd_table[tfd] : NULL;
        if (!wf) return (uint64_t)-EBADF;
        // Grow if at 75% load.
        if (state->count * 4u >= state->cap * 3u) {
            if (ep_grow(state, state->cap * 2u) < 0) return (uint64_t)-ENOMEM;
        }
        epoll_watch_t* w = (epoll_watch_t*)kmalloc(sizeof(epoll_watch_t));
        if (!w) return (uint64_t)-ENOMEM;
        w->fd         = tfd;
        w->events     = ev.events;
        w->data       = ev.data;
        w->has_entry2 = 0;
        epoll_watch_register(state, w, wf);
        ep_ht_insert_raw(state->slots, state->cap, w);
        state->count++;
        serial_puts_dbg("[epoll] ADD fd=");
        serial_hex_dbg((uint64_t)(uint32_t)tfd);
        return 0;
    }
    case EPOLL_CTL_DEL: {
        uint32_t idx = ep_find(state, tfd);
        if (idx >= state->cap) return (uint64_t)-ENOENT;
        epoll_watch_t* w = state->slots[idx];
        vfs_file_t* wf = ((uint32_t)tfd < files->fd_capacity)
                         ? files->fd_table[tfd] : NULL;
        epoll_watch_unregister(w, wf);
        state->slots[idx] = EPOLL_DELETED;
        state->count--;
        kfree(w);
        serial_puts_dbg("[epoll] DEL fd=");
        serial_hex_dbg((uint64_t)(uint32_t)tfd);
        return 0;
    }
    case EPOLL_CTL_MOD: {
        uint32_t idx = ep_find(state, tfd);
        if (idx >= state->cap) return (uint64_t)-ENOENT;
        epoll_watch_t* w = state->slots[idx];
        vfs_file_t* wf = ((uint32_t)tfd < files->fd_capacity)
                         ? files->fd_table[tfd] : NULL;
        epoll_watch_unregister(w, wf);
        w->events = ev.events;
        w->data   = ev.data;
        if (wf) epoll_watch_register(state, w, wf);
        return 0;
    }
    default:
        return (uint64_t)-EINVAL;
    }
}

// epoll_wait(epfd, events_ptr, maxevents, timeout_ms) → count or -errno
static uint64_t sys_epoll_wait(uint64_t epfd, uint64_t events_ptr,
                                uint64_t maxevents, uint64_t timeout_ms) {
    if (!g_current) return (uint64_t)-EINVAL;
    if (!events_ptr || maxevents == 0 || maxevents > 1024) return (uint64_t)-EINVAL;

    vfs_file_t* ef = fd_to_file(epfd);
    if (!ef || ef->close != epoll_close) return (uint64_t)-EBADF;
    epoll_state_t* state = (epoll_state_t*)ef->ctx;

    task_files_t* files = g_current->files_shared;
    epoll_event_t* uevents = (epoll_event_t*)events_ptr;

    int infinite = (timeout_ms == (uint64_t)-1);
    extern uint64_t tsc_read_ns(void);
    uint64_t timeout_ns = infinite ? 0 : (timeout_ms * 1000000ULL);
    uint64_t deadline   = infinite ? UINT64_MAX : (tsc_read_ns() + timeout_ns);
    int count = 0;

    // One task_we_t on the epoll's own wq — stack allocated, zero per-wakeup alloc.
    task_we_t task_we;

    do {
        count = 0;
        for (uint32_t i = 0; i < state->cap && (uint64_t)count < maxevents; i++) {
            epoll_watch_t* w = state->slots[i];
            if (!w || w == EPOLL_DELETED) continue;
            int32_t wfd = w->fd;

            vfs_file_t* f = ((uint32_t)wfd < files->fd_capacity)
                            ? files->fd_table[wfd] : NULL;
            if (!f) {
                epoll_event_t out;
                out.events = EPOLLERR | EPOLLHUP;
                out.data   = w->data;
                if (copy_to_user(uevents + count, &out, sizeof(out)) != 0)
                    return (uint64_t)-EFAULT;
                count++;
                continue;
            }

            uint32_t mask = w->events;
            uint32_t rev  = 0;
            if (f->poll) {
                if ((mask & EPOLLIN)  && f->poll(f, POLLIN))  rev |= EPOLLIN;
                if ((mask & EPOLLOUT) && f->poll(f, POLLOUT)) rev |= EPOLLOUT;
                if (f->poll(f, POLLHUP)) rev |= EPOLLHUP;
            } else {
                if (mask & EPOLLIN)  rev |= EPOLLIN;
                if (mask & EPOLLOUT) rev |= EPOLLOUT;
            }

            if (rev) {
                epoll_event_t out;
                out.events = rev;
                out.data   = w->data;
                if (copy_to_user(uevents + count, &out, sizeof(out)) != 0)
                    return (uint64_t)-EFAULT;
                count++;
            }
        }

        if (count > 0) break;
        if (timeout_ms == 0) break;

        // Sleep on epoll's own wq. Persistent epoll_we_t entries on each watched
        // fd's waitq call epoll_wake_func → set state->has_ready=1 and wake state->wq.
        // Zero per-wakeup allocation after epoll_ctl(ADD).
        state->has_ready = 0;
        task_we_init(&task_we, g_current);
        task_we_add(&state->wq, &task_we);

        // Re-check after registering to close the race window.
        int ep_recheck = state->has_ready;
        if (!ep_recheck) {
            for (uint32_t i = 0; i < state->cap && !ep_recheck; i++) {
                epoll_watch_t* w = state->slots[i];
                if (!w || w == EPOLL_DELETED) continue;
                int32_t wfd = w->fd;
                vfs_file_t* f = ((uint32_t)wfd < files->fd_capacity)
                                ? files->fd_table[wfd] : NULL;
                if (!f || !f->poll) continue;
                uint32_t mask = w->events;
                if ((mask & EPOLLIN)  && f->poll(f, POLLIN))  ep_recheck = 1;
                if ((mask & EPOLLOUT) && f->poll(f, POLLOUT)) ep_recheck = 1;
                if (f->poll(f, POLLHUP))                      ep_recheck = 1;
            }
        }
        if (!ep_recheck) {
            g_current->sleep_until_ns = infinite ? 0 : deadline;
            sched_sleep();
            g_current->sleep_until_ns = 0;
        }
        // Remove the task entry (wq_remove is a no-op if already removed by wake).
        task_we_remove(&state->wq, &task_we);

    } while (infinite || tsc_read_ns() < deadline);

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
// Sets file mode on ext2. If the setuid bit (04000) is being set by a root
// process, notifies ksec so it can auto-create a policy entry.
// The two-way invariant: bit set ⟺ ksec policy entry exists.
#define S_ISUID_BIT 04000u

static uint64_t sys_chmod(uint64_t path_ptr, uint64_t mode) {
    if (!g_current || !path_ptr) return (uint64_t)-EINVAL;

    // Only root may set the setuid bit (POSIX: non-root chmod silently clears it).
    if (!cred_is_root(&g_current->cred))
        mode &= ~(uint64_t)S_ISUID_BIT;

    // TODO: call ext2_chmod(path, mode) when ext2 mode storage is implemented.
    // For now: if setuid bit is being set and caller is root, notify ksec.
    if ((mode & S_ISUID_BIT) && cred_is_root(&g_current->cred) &&
        ksec_agent_present()) {
        char raw[512]; const char* upath = (const char*)path_ptr;
        uint64_t i = 0;
        while (i < 511 && upath[i]) { raw[i] = upath[i]; i++; }
        raw[i] = '\0';

        ksec_request_t req = {0};
        req.seq        = ksec_next_seq();
        req.op         = KSEC_OP_SETUID_BIT;
        req.caller_pid = g_current->pid;
        req.caller_uid = g_current->cred.euid;
        req.caller_gid = g_current->cred.egid;
        req.file_uid   = 0;   // TODO: read from inode when ext2 uids are stored
        // resource field not used for SETUID_BIT (inode/dev used instead)
        // inode/dev: not available until ext2_stat is wired — send 0 for now.
        req.inode      = 0;
        req.dev        = 0;
        ksec_notify(&req);
    }
    return 0;
}

static uint64_t sys_fchmod(uint64_t fd, uint64_t mode) {
    if (!g_current) return (uint64_t)-EBADF;
    if (!cred_is_root(&g_current->cred))
        mode &= ~(uint64_t)S_ISUID_BIT;
    (void)fd; (void)mode;
    // TODO: lookup path from fd->path, call ext2_chmod.
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
    // Require write permission on the file itself.
    int tr_err = 0;
    uint32_t tr_ino = ext2_lookup_path(path, &g_current->cred, &tr_err);
    if (!tr_ino) return tr_err ? (uint64_t)(int64_t)tr_err : (uint64_t)-ENOENT;
    ext2_inode_t tr_inode;
    if (!ext2_read_inode(tr_ino, &tr_inode)) return (uint64_t)-ENOENT;
    inode_perm_t tr_ip = {
        .uid = tr_inode.i_uid, .gid = tr_inode.i_gid,
        .mode = tr_inode.i_mode & 0x1FF,
        .inode_nr = tr_ino, .dev = 0, .nosuid = 0,
    };
    int tpr = vfs_check_perm(&tr_ip, &g_current->cred, ACL_PERM_WRITE);
    if (tpr != 0) return (uint64_t)(int64_t)tpr;

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

    // shmem fd: resize the shmem object.
    extern void shmem_fd_close(vfs_file_t*);
    if (f->close == shmem_fd_close) {
        shmem_t* shm = (shmem_t*)f->ctx;
        if (!shm) return (uint64_t)-EINVAL;
        uint32_t npages = (uint32_t)((length + PAGE_SIZE - 1) / PAGE_SIZE);
        int rc = shmem_resize(shm, npages);
        return (rc < 0) ? (uint64_t)rc : 0;
    }

    // Regular file.
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
static void serial_hex_u32(uint32_t v) {
    const char* h = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4) serial_putc(h[(v >> i) & 0xF]);
}

// ── Syscall jump table ────────────────────────────────────────────────────
// Every handler has the uniform signature (arg1, arg2, arg3, arg4).  Handlers
// that need arg5/arg6 read them from g_syscall_arg5/6 directly (set by the
// asm entry stub before calling us).  Handlers that take fewer args simply
// ignore the extras.  NULL means "not implemented" → ENOSYS.

typedef uint64_t (*sys_handler_t)(uint64_t, uint64_t, uint64_t, uint64_t);

// ── Thin wrappers for handlers with non-uniform signatures ───────────────

static uint64_t w_sys_write(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_write(a, b, c);
}
static uint64_t w_sys_exit(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d;
    if (g_current && g_current->pid > 2) {
        serial_puts("[exit] pid="); serial_hex_u32((uint32_t)g_current->pid);
        serial_puts(" code="); serial_hex_u32((uint32_t)a);
        serial_putc('\n');
    }
    return sys_exit(a);
}
static uint64_t w_sys_open(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d;
    uint64_t ret = sys_open(a, b, c);
    if (g_current && g_current->pid > 2) {
        serial_puts("[open] pid="); serial_hex_u32((uint32_t)g_current->pid);
        serial_putc(' ');
        serial_puts((const char*)a);
        serial_puts(" -> ");
        if ((int64_t)ret < 0) { serial_puts("ERR "); serial_hex_u32((uint32_t)(-(int64_t)ret)); }
        else { serial_puts("fd="); serial_hex_u32((uint32_t)ret); }
        serial_putc('\n');
    }
    return ret;
}
static uint64_t w_sys_close(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_close(a);
}
static uint64_t w_sys_brk(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_brk(a);
}
static uint64_t w_sys_kill(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_kill(a, b);
}
static uint64_t w_sys_fork(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_fork();
}
static uint64_t w_sys_exec(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_exec(a, b, c);
}
static uint64_t w_sys_wait(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_wait(a, b, c);
}
static uint64_t w_sys_getpid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_getpid();
}
static uint64_t w_sys_getppid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_getppid();
}
static uint64_t w_sys_spawn(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_spawn(a, b, c, d, g_syscall_arg5);
}
static uint64_t w_sys_thread(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_thread(a, b, c);
}
static uint64_t w_sys_clock_ns(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return tsc_read_ns();
}
static uint64_t w_sys_stat(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_stat(a, b, c);
}
static uint64_t w_sys_unlink(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_unlink(a, b);
}
static uint64_t w_sys_getcwd(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_getcwd(a, b);
}
static uint64_t w_sys_chdir(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_chdir(a, b);
}
static uint64_t w_sys_mkdir(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_mkdir(a, b);
}
static uint64_t w_sys_lseek(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_lseek(a, b, c);
}
static uint64_t w_sys_dup(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_dup(a);
}
static uint64_t w_sys_dup2(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_dup2(a, b);
}
static uint64_t w_sys_pipe(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_pipe(a);
}
static uint64_t w_sys_sigaction(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_sigaction(a, b, c);
}
static uint64_t w_sys_sigprocmask(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_sigprocmask(a, b, c);
}
static uint64_t w_sys_sigreturn(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_sigreturn();
}
static uint64_t w_sys_mmap(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_mmap(a, b, c, d, g_syscall_arg5, g_syscall_arg6);
}
static uint64_t w_sys_munmap(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_munmap(a, b);
}
static uint64_t w_sys_nanosleep(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_nanosleep(a, b);
}
static uint64_t w_sys_gettod(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_gettimeofday(a, b);
}
static uint64_t w_sys_fb_blit(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_fb_blit(a, b, c, d);
}
static uint64_t w_sys_fb_info(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_fb_info(a);
}
static uint64_t w_sys_fb_map(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_fb_map();
}
static uint64_t w_sys_openpty(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_openpty(a);
}
static uint64_t w_sys_getpeerpid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_getpeerpid(a);
}
static uint64_t w_sys_socket(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_socket(a, b, c);
}
static uint64_t w_sys_bind(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_bind(a, b, c);
}
static uint64_t w_sys_listen(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_listen(a, b);
}
static uint64_t w_sys_accept(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_accept(a, b, c);
}
static uint64_t w_sys_connect(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_connect(a, b, c);
}
static uint64_t w_sys_sendto(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_sendto(a, b, c, d, g_syscall_arg5, g_syscall_arg6);
}
static uint64_t w_sys_recvfrom(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_recvfrom(a, b, c, d, g_syscall_arg5, g_syscall_arg6);
}
static uint64_t w_sys_setsockopt(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_setsockopt(a, b, c, d, g_syscall_arg5);
}
static uint64_t w_sys_shutdown(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_shutdown(a, b);
}
static uint64_t w_sys_net_ifconfig(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_net_ifconfig(a, b);
}
static uint64_t w_sys_net_mac(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_net_mac(a);
}
static uint64_t w_sys_fcntl(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_fcntl(a, b, c);
}
static uint64_t w_sys_fstat(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_fstat(a, b);
}
static uint64_t w_sys_access(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_access(a, b);
}
static uint64_t w_sys_uname(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_uname(a);
}
static uint64_t w_sys_umask(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_umask(a);
}
static uint64_t w_sys_getuid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_getuid();
}
static uint64_t w_sys_geteuid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_geteuid();
}
static uint64_t w_sys_getgid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_getgid();
}
static uint64_t w_sys_getegid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_getegid();
}
static uint64_t w_sys_getgroups(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_getgroups(a, b);
}
static uint64_t w_sys_setpgid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_setpgid(a, b);
}
static uint64_t w_sys_getpgid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_getpgid(a);
}
static uint64_t w_sys_getpgrp(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_getpgrp();
}
static uint64_t w_sys_setsid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_setsid();
}
static uint64_t w_sys_getsid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_getsid(a);
}
static uint64_t w_sys_ioctl(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_ioctl(a, b, c);
}
static uint64_t w_sys_select(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_select(a, b, c, d, g_syscall_arg5);
}
static uint64_t w_sys_poll(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_poll(a, b, c);
}
static uint64_t w_sys_readlink(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_readlink(a, b, c);
}
static uint64_t w_sys_symlink(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_symlink(a, b);
}
static uint64_t w_sys_link(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_link(a, b);
}
static uint64_t w_sys_chmod(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_chmod(a, b);
}
static uint64_t w_sys_fchmod(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_fchmod(a, b);
}
static uint64_t w_sys_chown(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_chown(a, b, c);
}
static uint64_t w_sys_fchown(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_fchown(a, b, c);
}
static uint64_t w_sys_truncate(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_truncate(a, b);
}
static uint64_t w_sys_ftruncate(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_ftruncate(a, b);
}
static uint64_t w_sys_times(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_times(a);
}
static uint64_t w_sys_getrusage(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_getrusage(a, b);
}
static uint64_t w_sys_tcgetpgrp(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_tcgetpgrp(a);
}
static uint64_t w_sys_tcsetpgrp(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_tcsetpgrp(a, b);
}
static uint64_t w_sys_reboot(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d;
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("cli; hlt");
    return 0;
}
static uint64_t w_sys_setuid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_setuid(a);
}
static uint64_t w_sys_setgid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_setgid(a);
}
static uint64_t w_sys_seteuid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_seteuid(a);
}
static uint64_t w_sys_setegid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_setegid(a);
}
static uint64_t w_sys_setreuid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_setreuid(a, b);
}
static uint64_t w_sys_setregid(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_setregid(a, b);
}
static uint64_t w_sys_setgroups(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_setgroups(a, b);
}
static uint64_t w_sys_pledge(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_pledge(a);
}
static uint64_t w_sys_unveil(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_unveil(a, b, c);
}
static uint64_t w_sys_unveil_lock(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return sys_unveil_lock();
}
static uint64_t w_sys_restrict_fd(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_restrict_fd(a, b);
}
static uint64_t w_sys_sendfd(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)d; return sys_sendfd(a, b, c);
}
static uint64_t w_sys_recvfd(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_recvfd(a);
}
static uint64_t w_sys_register_policy_agent(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_register_policy_agent(a, b);
}
static uint64_t w_sys_shm_open(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_shm_open(a, b, c, d);
}
static uint64_t w_sys_shm_unlink(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)c; (void)d; return sys_shm_unlink(a, b);
}
static uint64_t w_sys_epoll_create(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return sys_epoll_create(a);
}
static uint64_t w_sys_epoll_ctl(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_epoll_ctl(a, b, c, d);
}
static uint64_t w_sys_epoll_wait(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    return sys_epoll_wait(a, b, c, d);
}

// 4-arg handlers that already match the uniform signature — used directly.
// sys_read(a,b,c,d), sys_readdir(a,b,c,d), sys_rename(a,b,c,d)

// ── The jump table ────────────────────────────────────────────────────────
// Indexed by syscall number.  NULL entries → ENOSYS.  Designated
// initializers mean any unused slot is implicitly NULL and reordering the
// array is safe.

static const sys_handler_t s_syscall_table[100] = {
    [SYS_WRITE]               = w_sys_write,
    [SYS_EXIT]                = w_sys_exit,
    [SYS_READ]                = sys_read,
    [SYS_OPEN]                = w_sys_open,
    [SYS_CLOSE]               = w_sys_close,
    [SYS_BRK]                 = w_sys_brk,
    [SYS_KILL]                = w_sys_kill,
    [SYS_FORK]                = w_sys_fork,
    [SYS_EXEC]                = w_sys_exec,
    [SYS_WAIT]                = w_sys_wait,
    [SYS_GETPID]              = w_sys_getpid,
    [SYS_READDIR]             = sys_readdir,
    [SYS_SPAWN]               = w_sys_spawn,
    [SYS_THREAD]              = w_sys_thread,
    [SYS_CLOCK_NS]            = w_sys_clock_ns,
    [SYS_STAT]                = w_sys_stat,
    [SYS_UNLINK]              = w_sys_unlink,
    [SYS_RENAME]              = sys_rename,
    [SYS_GETCWD]              = w_sys_getcwd,
    [SYS_CHDIR]               = w_sys_chdir,
    [SYS_MKDIR]               = w_sys_mkdir,
    [SYS_LSEEK]               = w_sys_lseek,
    [SYS_GETPPID]             = w_sys_getppid,
    [SYS_DUP]                 = w_sys_dup,
    [SYS_DUP2]                = w_sys_dup2,
    [SYS_PIPE]                = w_sys_pipe,
    [SYS_SIGACTION]           = w_sys_sigaction,
    [SYS_SIGPROCMASK]         = w_sys_sigprocmask,
    [SYS_SIGRETURN]           = w_sys_sigreturn,
    [SYS_MMAP]                = w_sys_mmap,
    [SYS_MUNMAP]              = w_sys_munmap,
    [SYS_NANOSLEEP]           = w_sys_nanosleep,
    [SYS_GETTOD]              = w_sys_gettod,
    [SYS_FB_BLIT]             = w_sys_fb_blit,
    [SYS_FB_INFO]             = w_sys_fb_info,
    [SYS_SOCKET]              = w_sys_socket,
    [SYS_BIND]                = w_sys_bind,
    [SYS_LISTEN]              = w_sys_listen,
    [SYS_ACCEPT]              = w_sys_accept,
    [SYS_CONNECT]             = w_sys_connect,
    [SYS_SENDTO]              = w_sys_sendto,
    [SYS_RECVFROM]            = w_sys_recvfrom,
    [SYS_SETSOCKOPT]          = w_sys_setsockopt,
    [SYS_SHUTDOWN]            = w_sys_shutdown,
    [SYS_FCNTL]               = w_sys_fcntl,
    [SYS_FSTAT]               = w_sys_fstat,
    [SYS_ACCESS]              = w_sys_access,
    [SYS_UNAME]               = w_sys_uname,
    [SYS_UMASK]               = w_sys_umask,
    [SYS_GETUID]              = w_sys_getuid,
    [SYS_GETEUID]             = w_sys_geteuid,
    [SYS_GETGID]              = w_sys_getgid,
    [SYS_GETEGID]             = w_sys_getegid,
    [SYS_GETGROUPS]           = w_sys_getgroups,
    [SYS_SETPGID]             = w_sys_setpgid,
    [SYS_GETPGID]             = w_sys_getpgid,
    [SYS_GETPGRP]             = w_sys_getpgrp,
    [SYS_SETSID]              = w_sys_setsid,
    [SYS_GETSID]              = w_sys_getsid,
    [SYS_IOCTL]               = w_sys_ioctl,
    [SYS_SELECT]              = w_sys_select,
    [SYS_POLL]                = w_sys_poll,
    [SYS_READLINK]            = w_sys_readlink,
    [SYS_SYMLINK]             = w_sys_symlink,
    [SYS_LINK]                = w_sys_link,
    [SYS_CHMOD]               = w_sys_chmod,
    [SYS_FCHMOD]              = w_sys_fchmod,
    [SYS_CHOWN]               = w_sys_chown,
    [SYS_FCHOWN]              = w_sys_fchown,
    [SYS_TRUNCATE]            = w_sys_truncate,
    [SYS_FTRUNCATE]           = w_sys_ftruncate,
    [SYS_TIMES]               = w_sys_times,
    [SYS_GETRUSAGE]           = w_sys_getrusage,
    [SYS_TCGETPGRP]           = w_sys_tcgetpgrp,
    [SYS_TCSETPGRP]           = w_sys_tcsetpgrp,
    [SYS_REBOOT]              = w_sys_reboot,
    [SYS_SETUID]              = w_sys_setuid,
    [SYS_SETGID]              = w_sys_setgid,
    [SYS_SETEUID]             = w_sys_seteuid,
    [SYS_SETEGID]             = w_sys_setegid,
    [SYS_SETREUID]            = w_sys_setreuid,
    [SYS_SETREGID]            = w_sys_setregid,
    [SYS_SETGROUPS]           = w_sys_setgroups,
    [SYS_PLEDGE]              = w_sys_pledge,
    [SYS_UNVEIL]              = w_sys_unveil,
    [SYS_UNVEIL_LOCK]         = w_sys_unveil_lock,
    [SYS_RESTRICT_FD]         = w_sys_restrict_fd,
    [SYS_SENDFD]              = w_sys_sendfd,
    [SYS_RECVFD]              = w_sys_recvfd,
    [SYS_REGISTER_POLICY_AGENT]= w_sys_register_policy_agent,
    [SYS_SHM_OPEN]            = w_sys_shm_open,
    [SYS_SHM_UNLINK]          = w_sys_shm_unlink,
    [SYS_FB_MAP]              = w_sys_fb_map,
    [SYS_OPENPTY]             = w_sys_openpty,
    [SYS_GETPEERPID]          = w_sys_getpeerpid,
    [SYS_NET_IFCONFIG]        = w_sys_net_ifconfig,
    [SYS_NET_MAC]             = w_sys_net_mac,
    [SYS_EPOLL_CREATE]        = w_sys_epoll_create,
    [SYS_EPOLL_CTL]           = w_sys_epoll_ctl,
    [SYS_EPOLL_WAIT]          = w_sys_epoll_wait,
};

// ── native_syscall_dispatch ───────────────────────────────────────────────
// Dispatches using our internal syscall numbers via a jump table.
uint64_t native_syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2,
                                  uint64_t arg3, uint64_t arg4) {
    uint64_t ret;
    // Mirror stdout/stderr writes to serial for debugging.
    if (nr == SYS_WRITE && (arg1 == 1 || arg1 == 2) && arg3 > 0) {
        const char* s = (const char*)arg2;
        for (uint64_t i = 0; i < arg3; i++) serial_putc(s[i]);
    }

    sys_handler_t h = (nr < (uint64_t)(sizeof(s_syscall_table) / sizeof(s_syscall_table[0])))
                      ? s_syscall_table[nr] : NULL;
    ret = h ? h(arg1, arg2, arg3, arg4) : (uint64_t)-ENOSYS;

    // Deliver any pending signals on the syscall return path.
    // g_signal_in_syscall=1 tells signal_deliver_pending it may set up user frames.
    g_signal_in_syscall = 1;
    signal_deliver_pending();
    g_signal_in_syscall = 0;
    return ret;
}

// ── syscall_dispatch ──────────────────────────────────────────────────────
// Entry point from syscall_entry.asm.
// Enforces pledge() before dispatching any handler.
uint64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4) {
    // ── Pledge enforcement ────────────────────────────────────────────────
    // Check before ANY handler runs.  Violation → SIGKILL (immediate, no
    // handler, no audit trail beyond the kill event itself).
    if (g_current && g_current->pledge_mask != PLEDGE_ALL) {
        uint32_t need = pledge_group_for_syscall(nr);
        if (need != 0 && !pledge_allowed(g_current->pledge_mask, need)) {
            signal_send(g_current, SIGKILL);
            // signal_deliver_pending will run on return; return a value that
            // will never be observed (process is dying).
            return (uint64_t)-EPERM;
        }
        // SYS_OPEN: finer-grained pledge check based on access mode.
        if (nr == SYS_OPEN) {
            int oflags = (int)(arg2 & 3);   // O_RDONLY=0, O_WRONLY=1, O_RDWR=2
            int creat  = (arg2 & O_CREAT) != 0;
            uint32_t need_open = 0;
            if (oflags == O_RDONLY && !creat) need_open = PLEDGE_RPATH;
            else if (creat)                   need_open = PLEDGE_CPATH;
            else                              need_open = PLEDGE_WPATH;
            if (!pledge_allowed(g_current->pledge_mask, need_open)) {
                signal_send(g_current, SIGKILL);
                return (uint64_t)-EPERM;
            }
        }
        // SYS_MMAP: PROT_EXEC requires PLEDGE_PROT_EXEC.
        if (nr == SYS_MMAP) {
            uint64_t prot = arg3;
            if ((prot & 4 /* PROT_EXEC */) &&
                !pledge_allowed(g_current->pledge_mask, PLEDGE_PROT_EXEC)) {
                signal_send(g_current, SIGKILL);
                return (uint64_t)-EPERM;
            }
        }
        // SYS_SOCKET: AF_INET vs AF_UNIX distinction.
        if (nr == SYS_SOCKET) {
            int domain = (int)arg1;
            uint32_t need_sock = (domain == 1 /* AF_UNIX */) ? PLEDGE_UNIX : PLEDGE_INET;
            if (!pledge_allowed(g_current->pledge_mask, need_sock)) {
                signal_send(g_current, SIGKILL);
                return (uint64_t)-EPERM;
            }
        }
        // SYS_IOCTL: tty vs non-tty.
        if (nr == SYS_IOCTL) {
            // Requests ≥ 0x5400 are tty ioctls.
            uint64_t req = arg2;
            uint32_t need_ioctl = (req >= 0x5400 && req <= 0x54FF)
                                  ? PLEDGE_TTY : PLEDGE_IOCTL;
            if (!pledge_allowed(g_current->pledge_mask, need_ioctl)) {
                signal_send(g_current, SIGKILL);
                return (uint64_t)-EPERM;
            }
        }
    }

    return native_syscall_dispatch(nr, arg1, arg2, arg3, arg4);
}
