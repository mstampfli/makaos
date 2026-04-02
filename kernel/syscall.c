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

// g_vga is defined in idt.c — also used by vfs.c.
extern volatile uint16_t* g_vga;

// Scratch storage for the user RSP during syscall entry.
uint64_t g_syscall_user_rsp = 0;

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
    if (!g_current || fd >= VFS_MAX_FDS) return (uint64_t)-1;
    vfs_file_t* f = g_current->fd_table[fd];
    if (!f) return (uint64_t)-1;
    int64_t r = vfs_write(f, (const void*)buf, len);
    return (r < 0) ? (uint64_t)-1 : (uint64_t)r;
}

// ── sys_read ──────────────────────────────────────────────────────────────
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags) {
    (void)flags;
    if (!g_current || fd >= VFS_MAX_FDS) return (uint64_t)-1;
    vfs_file_t* f = g_current->fd_table[fd];
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
    for (int fd = 3; fd < VFS_MAX_FDS; fd++) {
        if (!g_current->fd_table[fd]) {
            g_current->fd_table[fd] = f;
            return (uint64_t)fd;
        }
    }

    // No free slot — close the file we just opened.
    vfs_close(f);
    return (uint64_t)-1;
}

// ── sys_close ─────────────────────────────────────────────────────────────
// close(fd) → 0 or -1
static uint64_t sys_close(uint64_t fd) {
    if (!g_current) return (uint64_t)-1;
    if (fd < 3 || fd >= VFS_MAX_FDS) return (uint64_t)-1;  // protect 0/1/2
    vfs_file_t* f = g_current->fd_table[fd];
    if (!f) return (uint64_t)-1;
    vfs_close(f);
    g_current->fd_table[fd] = NULL;
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
    if (!g_current || !g_current->mm) return (uint64_t)-1;

    mm_t* mm = g_current->mm;

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
        // Find task by pid — walk queue + check current.
        task_t* target = NULL;
        if (g_current && (int64_t)g_current->pid == pid)
            target = g_current;
        if (!target) {
            task_t* t = sched_queue_head();
            while (t) {
                if ((int64_t)t->pid == pid) { target = t; break; }
                t = t->next;
            }
        }
        if (!target) return (uint64_t)-1;
        signal_send(target, sig);
        return 0;
    }

    if (pid == 0) {
        // Send to own thread group.
        if (!g_current) return (uint64_t)-1;
        signal_send_group(g_current->tgid, sig);
        return 0;
    }

    // pid == -1: broadcast (all non-kernel tasks).
    if (g_current && !(g_current->flags & TASK_FLAG_KTHREAD))
        signal_send(g_current, sig);
    task_t* t = sched_queue_head();
    while (t) {
        if (!(t->flags & TASK_FLAG_KTHREAD))
            signal_send(t, sig);
        t = t->next;
    }
    return 0;
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
        case SYS_WRITE: ret = sys_write(arg1, arg2, arg3); break;
        case SYS_EXIT:  ret = sys_exit(arg1);               break;
        case SYS_READ:  ret = sys_read(arg1, arg2, arg3, arg4); break;
        case SYS_OPEN:  ret = sys_open(arg1, arg2);         break;
        case SYS_CLOSE: ret = sys_close(arg1);              break;
        case SYS_BRK:   ret = sys_brk(arg1);                break;
        case SYS_KILL:  ret = sys_kill(arg1, arg2);         break;
        default:        ret = (uint64_t)-1;                 break;
    }
    // Deliver any signals that were queued during or before this syscall.
    signal_deliver_pending();
    return ret;
}
