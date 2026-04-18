#include "common.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "pcache.h"
#include "kheap.h"
#include "tss.h"
#include "syscall.h"
#include "process.h"
#include "pic.h"
#include "timer.h"
#include "sched.h"
#include "cpu.h"
#include "smp_boot.h"
#include "irq_wait.h"
#include "ahci.h"
#include "nvme.h"
#include "ext2.h"
#include "elf.h"
#include "tsc.h"
#include "fb.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "tty.h"
#include "evdev.h"
#include "initcall.h"
#include "kheap.h"
#include "vfs.h"
#include "kprintf.h"

phys_addr_t KERNEL_BASE_PHYS     = 0;
uint64_t    KERNEL_SIZE          = 0;
uint64_t    LOADER_RESERVED_SIZE = 0;

// Defined by the serial_*_dbg helpers in common.h.  Plain BSS init
// (zero) is already the unlocked state, so no explicit initialization
// is required before the first serial print.
volatile uint32_t g_serial_lock = 0;

extern void lapic_spurious_entry(void);
extern void ipi_reschedule_entry(void);
extern void ipi_call_entry(void);
extern void ipi_tlb_flush_entry(void);

static void serial_init_and_say(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, 'K');
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, '\n');
}

// ── stress_pread — hammer AHCI pread path to catch data corruption ───────
// One-boot stress: reads /bin/bash in full once as a reference, then
// does N random 4 KiB pread()s and byte-compares each against the
// reference.  Any short read, error, or data mismatch is printed with
// enough context to diagnose.  Runs in parallel with login/bash so the
// AHCI slot contention is realistic.

#define STRESS_PREAD_ITERS 10000
#define STRESS_PREAD_WORKERS 4

static volatile uint32_t g_stress_done_count = 0;

static void stress_pread_worker(void) {
    vfs_file_t* f = ext2_open("/bin/bash");
    if (!f) { kprintf("[stress] open /bin/bash failed\n");
             __atomic_fetch_add(&g_stress_done_count, 1, __ATOMIC_RELAXED);
             g_current->state = TASK_DEAD; sched_yield(); for(;;) __asm__("hlt"); }

    int64_t size = f->seek(f, 0, SEEK_END);
    f->seek(f, 0, SEEK_SET);
    if (size <= 0) { kprintf("[stress] size<=0 %lu\n", (uint64_t)size);
                     f->close(f);
                     __atomic_fetch_add(&g_stress_done_count, 1, __ATOMIC_RELAXED);
                     g_current->state = TASK_DEAD; sched_yield(); for(;;) __asm__("hlt"); }

    uint8_t* ref = kmalloc((uint64_t)size);
    int64_t nr = f->pread(f, ref, (uint64_t)size, 0);
    if (nr != size) {
        kprintf("[stress] pid=%u ref-read short: %lu/%lu\n",
                g_current->pid, (uint64_t)nr, (uint64_t)size);
        kfree(ref); f->close(f);
        __atomic_fetch_add(&g_stress_done_count, 1, __ATOMIC_RELAXED);
        g_current->state = TASK_DEAD; sched_yield(); for(;;) __asm__("hlt");
    }

    uint8_t* tmp = kmalloc(4096);
    uint64_t seed = 0xC0FFEE00DEADBEEFULL ^ ((uint64_t)g_current->pid << 16);
    uint64_t fails = 0, shorts = 0, errs = 0;
    uint64_t pages = ((uint64_t)size + 4095) / 4096;

    kprintf("[stress] pid=%u START size=%lu pages=%lu\n",
            g_current->pid, (uint64_t)size, pages);

    for (uint64_t i = 0; i < STRESS_PREAD_ITERS; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t pg  = (seed >> 16) % pages;
        uint64_t off = pg * 4096;
        uint64_t want = ((uint64_t)size - off) < 4096 ? (uint64_t)size - off : 4096;

        int64_t got = f->pread(f, tmp, want, off);
        if (got < 0) {
            errs++;
            kprintf("[stress] pid=%u i=%lu pread ERR=%lu off=%lx\n",
                    g_current->pid, i, (uint64_t)got, off);
            continue;
        }
        if ((uint64_t)got != want) {
            shorts++;
            kprintf("[stress] pid=%u i=%lu SHORT got=%lu want=%lu off=%lx\n",
                    g_current->pid, i, (uint64_t)got, want, off);
            continue;
        }
        uint64_t first = (uint64_t)-1;
        for (uint64_t j = 0; j < want; j++)
            if (tmp[j] != ref[off + j]) { first = j; break; }
        if (first != (uint64_t)-1) {
            fails++;
            kprintf("[stress] pid=%u i=%lu MISMATCH off=%lx first_diff=+%lu "
                    "got=%x want=%x\n",
                    g_current->pid, i, off, first,
                    (uint32_t)tmp[first], (uint32_t)ref[off + first]);
        }
    }

    kprintf("[stress] pid=%u DONE iters=%u mismatches=%lu shorts=%lu errs=%lu\n",
            g_current->pid, (uint32_t)STRESS_PREAD_ITERS,
            fails, shorts, errs);

    kfree(tmp); kfree(ref); f->close(f);
    __atomic_fetch_add(&g_stress_done_count, 1, __ATOMIC_RELAXED);
    g_current->state = TASK_DEAD; sched_yield();
    for (;;) __asm__ volatile("hlt");
}

static void stress_pread_launch(void) {
    kprintf("[stress] launching %d workers x %d pread iters each\n",
            STRESS_PREAD_WORKERS, STRESS_PREAD_ITERS);
    for (int i = 0; i < STRESS_PREAD_WORKERS; i++) {
        task_t* t = task_create_kthread(stress_pread_worker, pid_alloc());
        if (t) sched_add(t);
    }
}

// ── stress_nvme — hammer the NVMe I/O path ──────────────────────────────
// At boot, reads the first NVME_STRESS_REF_PAGES pages of the NVMe disk
// sequentially (single-threaded) into an in-memory reference.  Then
// spawns NVME_STRESS_WORKERS kthreads, each doing NVME_STRESS_ITERS
// random 4 KiB reads and byte-comparing against the reference.  Any
// mismatch is a bug in the NVMe submit/completion path.

#define NVME_STRESS_REF_PAGES 256      // 256 × 4 KiB = 1 MiB reference
#define NVME_STRESS_ITERS     10000
#define NVME_STRESS_WORKERS   4  // fixed — was blocked by kstack_alloc race

static uint8_t* g_nvme_ref  = NULL;
static uint64_t g_nvme_pages = 0;
static uint8_t* g_nvme_tmp_bufs[16] = {0};  // pre-allocated, one per worker
static volatile uint32_t g_nvme_tmp_ix = 0;

static void stress_nvme_worker(void) {
    uint32_t my_ix = __atomic_fetch_add(&g_nvme_tmp_ix, 1, __ATOMIC_RELAXED);
    uint8_t* tmp = g_nvme_tmp_bufs[my_ix];
    uint64_t seed = 0xA5A5DEADBEEFCAFEULL ^ ((uint64_t)g_current->pid << 16);
    uint64_t fails = 0, errs = 0;

    kprintf("[nvme-stress] pid=%u START pages=%lu\n",
            g_current->pid, g_nvme_pages);

    for (uint64_t i = 0; i < NVME_STRESS_ITERS; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t pg  = (seed >> 16) % g_nvme_pages;
        uint64_t lba = pg * 8;  // 4K page = 8 × 512 B LBAs

        if (!nvme_read(lba, tmp, 8)) {
            errs++;
            kprintf("[nvme-stress] pid=%u i=%lu READ FAILED lba=%lx\n",
                    g_current->pid, i, lba);
            continue;
        }
        uint8_t* ref = g_nvme_ref + pg * 4096;
        uint64_t first = (uint64_t)-1;
        for (uint64_t j = 0; j < 4096; j++)
            if (tmp[j] != ref[j]) { first = j; break; }
        if (first != (uint64_t)-1) {
            fails++;
            kprintf("[nvme-stress] pid=%u i=%lu MISMATCH lba=%lx "
                    "first_diff=+%lu got=%x want=%x\n",
                    g_current->pid, i, lba, first,
                    (uint32_t)tmp[first], (uint32_t)ref[first]);
        }
    }

    kprintf("[nvme-stress] pid=%u DONE iters=%u mismatches=%lu errs=%lu\n",
            g_current->pid, (uint32_t)NVME_STRESS_ITERS, fails, errs);

    // tmp is from g_nvme_tmp_bufs[] — freed by launch harness.
    g_current->state = TASK_DEAD; sched_yield();
    for (;;) __asm__ volatile("hlt");
}

static void stress_nvme_launch(void) {
    // Build the reference by reading the first N pages SINGLE-THREADED.
    uint64_t ref_bytes = (uint64_t)NVME_STRESS_REF_PAGES * 4096;
    uint8_t* ref = (uint8_t*)kmalloc(ref_bytes);
    if (!ref) { kprintf("[nvme-stress] OOM ref\n"); return; }

    kprintf("[nvme-stress] building reference (%lu pages, %lu bytes)\n",
            (uint64_t)NVME_STRESS_REF_PAGES, ref_bytes);
    for (uint64_t pg = 0; pg < NVME_STRESS_REF_PAGES; pg++) {
        if (!nvme_read(pg * 8, ref + pg * 4096, 8)) {
            kprintf("[nvme-stress] ref read failed at page %lu\n", pg);
            kfree(ref);
            return;
        }
    }
    g_nvme_ref = ref;
    g_nvme_pages = NVME_STRESS_REF_PAGES;

    // Pre-allocate worker tmp buffers as 4 KiB-aligned PMM pages so PRP1
    // covers the whole transfer with PRP2=0 (no cross-page assumption).
    for (int i = 0; i < NVME_STRESS_WORKERS; i++) {
        phys_addr_t p = pmm_buddy_alloc(0);
        g_nvme_tmp_bufs[i] = (uint8_t*)(p + HHDM_OFFSET);
    }
    g_nvme_tmp_ix = 0;

    kprintf("[nvme-stress] reference built; launching %u workers x %u iters\n",
            (uint32_t)NVME_STRESS_WORKERS, (uint32_t)NVME_STRESS_ITERS);

    for (int i = 0; i < NVME_STRESS_WORKERS; i++) {
        task_t* t = task_create_kthread(stress_nvme_worker, pid_alloc());
        if (t) sched_add(t);
    }
}

// ── init_kthread ──────────────────────────────────────────────────────────
// Runs in process context after the scheduler and timer are live.
// Calls do_initcalls_subsys() then spawns login and svcmgr.
// svcmgr owns all userspace services (reads /etc/services/*.svc).

static void init_kthread(void) {
    do_initcalls_subsys();

    // ahci_init() (called above via do_initcalls_subsys) has now set
    // g_ahci_irq.  ahci_start_io_thread() reads g_ahci_irq to decide
    // whether to arm the NCQ path; it must run AFTER ahci_init().
    ahci_start_io_thread();

    // Start the page cache reclaim kthread (CLOCK eviction).
    pcache_start_reclaim_thread();

    // Phase 9-4b/c: wake every AP discovered by ACPI now that the
    // scheduler, timers, LAPIC, VMM, and PMM are fully live.  APs
    // land in cpu_init_ap and drop into an idle-hlt loop until
    // Phase 9-5 wires them into the per-CPU scheduler tick.
    smp_boot_aps();

    // Load userspace processes sequentially — ext2 is not thread-safe.
    static const char* envp[] = { "PATH=/bin", "HOME=/root", "TERM=linux", NULL };

    static const char* login_argv[]  = { "/bin/login",  NULL };
    static const char* svcmgr_argv[] = { "/bin/svcmgr", NULL };

    static const int login_stdio[3]  = { -1, -1, -1 }; // inherit tty0
    static const int svcmgr_stdio[3] = { -2, -2, -2 }; // /dev/null

    task_t* login  = elf_exec_kernel("/bin/login",  pid_alloc(), login_argv,  envp, login_stdio);
    task_t* svcmgr = elf_exec_kernel("/bin/svcmgr", pid_alloc(), svcmgr_argv, envp, svcmgr_stdio);

    if (login)  sched_add(login);
    if (svcmgr) sched_add(svcmgr);

    // Regression-test harness: uncomment to stress AHCI concurrent pread.
    // Kept in-tree so future changes to the submit path can be validated.
    // stress_pread_launch();
    (void)stress_pread_launch;

    // NVMe stress: 4 kthreads × 10000 random 4 KiB reads vs reference.
    stress_nvme_launch();

    // init_kthread has finished its work (subsystem init, AP boot, userland
    // launch).  Previously, falling off the end landed in proc_trampoline's
    // `.dead: sti; hlt; jmp` loop — which kept this task TASK_RUNNING on
    // CPU 0 forever.  Every ~ms the timer tick re-queued it, so any user
    // task with home_cpu=0 fought init_kthread for scheduling slots.
    // Observed symptom: every 4th `ls` (round-robin puts one in four on
    // CPU 0) took ~400 ms inside vfs_read instead of ~5 ms.
    //
    // Park permanently: transition to TASK_DEAD and yield.  The next
    // do_switch on CPU 0 sees state != RUNNING, won't re-enqueue us, and
    // picks whichever user/kernel task is actually runnable there.
    g_current->state = TASK_DEAD;
    sched_yield();
    // Unreachable — do_switch reaps us via process_destroy.
    for (;;) __asm__ volatile("hlt");
}

// ── kmain ─────────────────────────────────────────────────────────────────
// Minimal boot sequence: BSS clear, serial, boot-info extraction,
// CPU feature setup, then do_initcalls_early() for all early subsystems,
// then hand off to the scheduler.

void kmain(void) {
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    serial_init_and_say();

    extern void* bootinfo_ptr;
    boot_info_t* info = (boot_info_t*)((uint64_t)bootinfo_ptr + HHDM_OFFSET);

    KERNEL_BASE_PHYS     = info->kernel_phys_base;
    KERNEL_SIZE          = (uint64_t)__kernel_end - (uint64_t)__kernel_start;
    LOADER_RESERVED_SIZE = 32ULL * 1024 * 1024;

    // ── CPU feature setup — must happen before any subsystem uses these ───
    // NXE bit in EFER (no-execute pages)
    {
        uint32_t efer_lo, efer_hi;
        __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080U));
        efer_lo |= (1U << 11);
        __asm__ volatile("wrmsr" : : "a"(efer_lo), "d"(efer_hi), "c"(0xC0000080U));
    }
    // PAT (Page Attribute Table): program MSR 0x277 so that PAT entry 1
    // (selected by PWT=1, PCD=0, PAT=0 in the PTE) is write-combining (WC).
    // Default layout: [0]=WB [1]=WT [2]=UC- [3]=UC [4]=WB [5]=WT [6]=UC- [7]=UC
    // We change entry 1 from WT (0x04) to WC (0x01) — only affects pages
    // mapped with PWT=1,PCD=0 which is exclusively the user framebuffer mapping.
    // MMIO mappings use PWT=1,PCD=1 → entry 3 (UC) — unchanged.
    {
        // PAT encoding: each entry is 3 bits in an 8-byte MSR.
        // Byte 0 = entry 0, byte 1 = entry 1, ..., byte 7 = entry 7.
        // Types: WB=0x06, WT=0x04, UC-=0x07, UC=0x00, WC=0x01
        uint32_t pat_lo = 0x00010406; // [3]=UC [2]=UC- [1]=WC [0]=WB  (changed [1] WT→WC)
        uint32_t pat_hi = 0x00070406; // [7]=UC [6]=UC- [5]=WT [4]=WB  (upper half unchanged)
        __asm__ volatile("wrmsr" : : "a"(pat_lo), "d"(pat_hi), "c"(0x277U));
    }
    // SSE/SSE2: CR4.OSFXSR + CR4.OSXMMEXCPT, clear CR0.EM + CR0.TS
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((uint64_t)(1 << 2));
        cr0 &= ~((uint64_t)(1 << 3));
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9);
        cr4 |= (1ULL << 10);
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    // ── Framebuffer — needed by early serial/debug output ────────────────
    fb_init(info->fb_phys, info->fb_width, info->fb_height, info->fb_pitch);

    // ── IDT — must be live before any other init raises exceptions ────────
    idt_init();

    // ── PIC — remap and eventually disable in favour of LAPIC/IOAPIC ─────
    pic_init(0x20, 0x28);

    // ── ACPI — parse tables so lapic/ioapic init can consume g_acpi ──────
    // The UEFI bootloader walked the EFI config tables and passed the
    // RSDP physical address to us in boot_info.  Pass it through so
    // acpi_parse doesn't have to guess.
    g_acpi = acpi_parse(info->rsdp_phys);
    if (!g_acpi.ok) {
        g_acpi.lapic_phys      = 0xFEE00000ULL;
        g_acpi.ioapic_phys     = 0xFEC00000ULL;
        g_acpi.ioapic_gsi_base = 0;
        g_acpi.override_count  = 0;
        g_acpi.ok              = 1;
    }

    // ── Memory — PMM then heap then VMM (strict dependency order) ────────
    tsc_init();
    pmm_buddy_init_from_map(info->e820_map, info->e820_count);
    kheap_init();
    vmm_init(info->pml4_phys);
    pcache_init();

    // ── Interrupt controllers ─────────────────────────────────────────────
    lapic_init(g_acpi.lapic_phys);
    ioapic_init(&g_acpi);
    pic_disable();
    idt_irq_register(VEC_LAPIC_SPURIOUS, (uint64_t)lapic_spurious_entry);
    // Phase 9-5: IPI vectors.  The IDT is shared across all CPUs, so
    // registering here is a one-time BSP step that every AP picks up
    // for free via idt_load_ap().
    idt_irq_register(VEC_IPI_RESCHEDULE, (uint64_t)ipi_reschedule_entry);
    idt_irq_register(VEC_IPI_CALL,       (uint64_t)ipi_call_entry);
    idt_irq_register(VEC_IPI_TLB_FLUSH,  (uint64_t)ipi_tlb_flush_entry);

    // ── CPU structures ────────────────────────────────────────────────────
    tss_init();
    syscall_init();
    // cpu_init_bsp programs GS_BASE.  It MUST run after tss_init: the
    // GDT-reload sequence inside tss_init writes 0 to %gs (a selector
    // load), which on x86-64 clears the GS_BASE MSR as a side effect.
    // Any wrmsr we did before tss_init would be wiped out.  Anything
    // before this line that calls this_cpu() / preempt_disable would
    // dereference %gs:0 == NULL → triple fault.  None of the early
    // boot subsystems need this_cpu(); they're verified clean.
    cpu_init_bsp();
    irq_wait_init();          // per-IRQ wait queues (Phase 3 SMP)

    // ── Scheduler + timer — sched_init must precede timer_init ───────────
    sched_init();
    timer_init(1000);
    __asm__ volatile("sti");

    // ── Early initcalls ───────────────────────────────────────────────────
    do_initcalls_early();

    // ── Hand off to init_kthread (subsys initcalls run there) ────────────
    task_t* kt = task_create_kthread(init_kthread, pid_alloc());
    if (!kt) for (;;) __asm__ volatile("hlt");
    sched_add(kt);

    for (;;)
        __asm__ volatile("hlt");
}
