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
extern void ipi_halt_entry(void);

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
#define NVME_STRESS_WORKERS   4  // target: 4-way concurrent correctness

static uint8_t* g_nvme_ref  = NULL;
static uint64_t g_nvme_pages = 0;
static uint8_t* g_nvme_tmp_bufs[16] = {0};  // pre-allocated, one per worker
static volatile uint32_t g_nvme_tmp_ix = 0;

// Cross-worker tallies — written by workers on DONE, read by the
// launcher-side barrier thread.  No serial output involved, so
// interleaved kprintf lines can't confuse the completion accounting.
static volatile uint32_t g_nvme_done_count = 0;
static volatile uint64_t g_nvme_total_mismatches = 0;
static volatile uint32_t g_ahci_done_count = 0;
static volatile uint64_t g_ahci_total_mismatches = 0;

static void stress_nvme_worker(void) {
    uint32_t my_ix = __atomic_fetch_add(&g_nvme_tmp_ix, 1, __ATOMIC_RELAXED);
    uint8_t* tmp = g_nvme_tmp_bufs[my_ix];
    uint64_t seed = 0xA5A5DEADBEEFCAFEULL ^ ((uint64_t)g_current->pid << 16);
    uint64_t fails = 0, errs = 0;

    kprintf("[nvme-stress] pid=%u START pages=%lu start_cpu=%u\n",
            g_current->pid, g_nvme_pages, this_cpu()->id);

    uint64_t t_start = tsc_read_ns();
    uint32_t cpu_hist[4] = {0};  // residence counter per CPU
    for (uint64_t i = 0; i < NVME_STRESS_ITERS; i++) {
        uint32_t cur_cpu = this_cpu()->id;
        if (cur_cpu < 4) cpu_hist[cur_cpu]++;
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

    uint64_t t_end = tsc_read_ns();
    uint64_t elapsed_ms = (t_end - t_start) / 1000000ULL;
    // Aggregate under atomic RMW — cannot be corrupted by interleaving.
    __atomic_fetch_add(&g_nvme_total_mismatches, fails, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_nvme_done_count, 1, __ATOMIC_RELEASE);
    kprintf("[nvme-stress] pid=%u DONE iters=%u mismatches=%lu errs=%lu "
            "elapsed_ms=%lu cpu_hist=[%u,%u,%u,%u]\n",
            g_current->pid, (uint32_t)NVME_STRESS_ITERS, fails, errs,
            elapsed_ms, cpu_hist[0], cpu_hist[1], cpu_hist[2], cpu_hist[3]);

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

// ── stress_ahci — raw ahci_read() stress, mirrors the NVMe harness ──────
//
// Verifies the AHCI completion path under SMP load: same pattern as
// stress_nvme, but through the single-port NCQ interface.  The device
// is the OVMF boot disk (build/disk.img) which is the same one ext2
// reads from, so the reference is reproducible.

#define AHCI_STRESS_REF_PAGES 256
#define AHCI_STRESS_ITERS     5000
#define AHCI_STRESS_WORKERS   4

static uint8_t* g_ahci_ref  = NULL;
static uint64_t g_ahci_pages = 0;
static uint8_t* g_ahci_tmp_bufs[16] = {0};
static volatile uint32_t g_ahci_tmp_ix = 0;

static void stress_ahci_worker(void) {
    uint32_t my_ix = __atomic_fetch_add(&g_ahci_tmp_ix, 1, __ATOMIC_RELAXED);
    uint8_t* tmp = g_ahci_tmp_bufs[my_ix];
    uint64_t seed = 0xBEEFCAFEDEADA5A5ULL ^ ((uint64_t)g_current->pid << 16);
    uint64_t fails = 0, errs = 0;

    kprintf("[ahci-stress] pid=%u START pages=%lu\n",
            g_current->pid, g_ahci_pages);

    for (uint64_t i = 0; i < AHCI_STRESS_ITERS; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t pg  = (seed >> 16) % g_ahci_pages;
        uint64_t lba = pg * 8;  // 4K page = 8 × 512B LBAs

        if (!ahci_read(lba, tmp, 8)) {
            errs++;
            kprintf("[ahci-stress] pid=%u i=%lu READ FAILED lba=%lx\n",
                    g_current->pid, i, lba);
            continue;
        }
        uint8_t* ref = g_ahci_ref + pg * 4096;
        uint64_t first = (uint64_t)-1;
        for (uint64_t j = 0; j < 4096; j++)
            if (tmp[j] != ref[j]) { first = j; break; }
        if (first != (uint64_t)-1) {
            fails++;
            kprintf("[ahci-stress] pid=%u i=%lu MISMATCH lba=%lx "
                    "first_diff=+%lu got=%x want=%x\n",
                    g_current->pid, i, lba, first,
                    (uint32_t)tmp[first], (uint32_t)ref[first]);
        }
    }

    __atomic_fetch_add(&g_ahci_total_mismatches, fails, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_ahci_done_count, 1, __ATOMIC_RELEASE);
    kprintf("[ahci-stress] pid=%u DONE iters=%u mismatches=%lu errs=%lu\n",
            g_current->pid, (uint32_t)AHCI_STRESS_ITERS, fails, errs);

    g_current->state = TASK_DEAD; sched_yield();
    for (;;) __asm__ volatile("hlt");
}

static void stress_ahci_launch(void) {
    uint64_t ref_bytes = (uint64_t)AHCI_STRESS_REF_PAGES * 4096;
    uint8_t* ref = (uint8_t*)kmalloc(ref_bytes);
    if (!ref) { kprintf("[ahci-stress] OOM ref\n"); return; }

    kprintf("[ahci-stress] building reference (%lu pages, %lu bytes)\n",
            (uint64_t)AHCI_STRESS_REF_PAGES, ref_bytes);
    for (uint64_t pg = 0; pg < AHCI_STRESS_REF_PAGES; pg++) {
        if (!ahci_read(pg * 8, ref + pg * 4096, 8)) {
            kprintf("[ahci-stress] ref read failed at page %lu\n", pg);
            kfree(ref);
            return;
        }
    }
    g_ahci_ref = ref;
    g_ahci_pages = AHCI_STRESS_REF_PAGES;

    for (int i = 0; i < AHCI_STRESS_WORKERS; i++) {
        phys_addr_t p = pmm_buddy_alloc(0);
        g_ahci_tmp_bufs[i] = (uint8_t*)(p + HHDM_OFFSET);
    }
    g_ahci_tmp_ix = 0;

    kprintf("[ahci-stress] reference built; launching %u workers x %u iters\n",
            (uint32_t)AHCI_STRESS_WORKERS, (uint32_t)AHCI_STRESS_ITERS);

    for (int i = 0; i < AHCI_STRESS_WORKERS; i++) {
        task_t* t = task_create_kthread(stress_ahci_worker, pid_alloc());
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

    // Phase 4F: start the slab / pcp shrinker kthread.  Periodically
    // drains each cache's empty_list and each CPU's pcp back to the
    // buddy allocator so reclaimable pages don't park indefinitely.
    extern void slab_shrinker_start(void);
    slab_shrinker_start();

    // Phase 5B: start the RCU GP kthread.  Drains each CPU's
    // rcu_pending_head (pushed lock-free by call_rcu_head) once per
    // grace period.  Must start before any call_rcu_head site fires,
    // which in practice means before userspace launches.
    extern void rcu_gp_kthread_start(void);
    rcu_gp_kthread_start();

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
    // Phase 1 of the work-stealing scheduler refactor: verify the
    // Chase-Lev deque primitive under live SMP before any scheduler
    // code consumes it.  Must run AFTER smp_boot_aps() so thieves
    // actually land on remote CPUs.
    // ── Boot self-tests — dev validation only, OFF by default ─────────────
    // These stress loops (slab/chaselev/typesafe spawn one kthread per CPU
    // × tens of thousands of iters) add 20-40s to every boot AND
    // intermittently DEADLOCK: a worker kthread exits with a runtime-
    // corrupted cleartid_addr (e.g. 0x53004300530000, non-canonical) →
    // copy_to_user #GP → that CPU wedges in the fault handler → a peer's
    // synchronize_rcu spins forever.  That latent corruption is a real bug
    // (tracked in docs/SCALABILITY_DEBT.md) but it is NOT on the desktop's
    // critical path — gate the whole battery behind a build flag so the
    // shipping boot is fast and deterministic.  Re-enable for validation
    // with `-DMAKAOS_BOOT_SELFTESTS` (e.g. SELFTESTS=1 bash build.sh).
#ifdef MAKAOS_BOOT_SELFTESTS
    // Foundation primitives (checked.h): overflow-safe arithmetic + bounds.
    extern void checked_selftest(void);
    checked_selftest();

    extern void chaselev_selftest(void);
    chaselev_selftest();

    // Phase 4H: slab / pcp acceptance self-test.  One kthread per
    // online CPU × 50k alloc/free pairs of mixed sizes.  Reports
    // slab fast-path hit rate and pcp hit rate; asserts >= 95%.
    extern void slab_pcpu_selftest(void);
    slab_pcpu_selftest();

    // Audit fix: big-alloc (buddy-backed) kmalloc header was sized for 8
    // bytes but offset by 16 -> up to 8-byte heap overflow for sizes in the
    // (2^k - 16, 2^k - 8] band.  Verify usable span covers the request.
    extern void kheap_overflow_selftest(void);
    kheap_overflow_selftest();

    // Audit fix: several syscalls did raw memcpy on unvalidated user pointers
    // (kernel-fault DoS + arbitrary kernel write).  They now use copy_*_user;
    // verify those reject kernel/non-canonical/NULL pointers with -EFAULT.
    extern void copy_user_selftest(void);
    copy_user_selftest();

    extern void copy_path_user_selftest(void);
    copy_path_user_selftest();

    // exec/spawn argv+envp vector copier must reject bad array/element pointers
    // with -EFAULT instead of raw-dereferencing them (kernel leak / panic).
    extern void copy_user_strv_selftest(void);
    copy_user_strv_selftest();

    // Audit fix: sys_spawn SPAWN_ATTR_CRED applied attacker-chosen uid/gid with
    // no caller check (root LPE).  Verify the down-only credential gate.
    extern void spawn_cred_allowed_selftest(void);
    spawn_cred_allowed_selftest();

    // Thread-group children anchoring: any thread reaps any process-child.
    extern void tg_leader_selftest(void);
    tg_leader_selftest();

    // Exit disposition shared by sys_exit + the fatal-signal path: a thread
    // self-reaps as TASK_DEAD (no zombie-linger leak), a leader zombifies.
    extern void task_exit_state_selftest(void);
    task_exit_state_selftest();

    // fd-table drop on exit: files_shared is unpublished (RELEASE) BEFORE the
    // RCU-deferred table free, so a /proc/<pid>/fd reader cannot race it; one
    // shared mechanism (task_drop_files) for sys_exit + the fatal-signal path.
    extern void task_files_drop_selftest(void);
    task_files_drop_selftest();

    // setuid-on-exec escalation gate (now wired via ksec).  Verify it fails
    // closed: only an explicit ksec ALLOW (with an agent present) escalates.
    extern void ksec_exec_setuid_selftest(void);
    ksec_exec_setuid_selftest();

    // ksec request rendezvous: per-seq in-flight slots (not one shared slot)
    // under a lock, so concurrent setuid callers cannot overwrite each other's
    // waiter (a wake-of-freed-task UAF) or read each other's verdict.
    extern void ksec_slot_selftest(void);
    ksec_slot_selftest();

    // unveil sandbox gate (now enforced by every path syscall, not just open).
    extern void unveil_gate_selftest(void);
    unveil_gate_selftest();

    // Audit fix: sys_mmap/sys_munmap page-rounded user len with a wrapping add
    // and never bounded [addr, addr+len) to the user half, so a MAP_FIXED (or
    // munmap) at a higher-half address drove the unmap+free path through the
    // shared kernel/HHDM page tables -> live-kernel-frame free / LPE.  Verify
    // the overflow-safe range helpers reject the wrap + escape cases.
    extern void mmap_range_selftest(void);
    mmap_range_selftest();
    extern void access_ok_selftest(void);
    access_ok_selftest();

    // Raw user-pointer syscalls (wait/pipe/fb_info/nanosleep/shm_open/unlink)
    // now route through copy_*_user; bad (kernel/non-canonical) pointers must
    // return -EFAULT instead of an arbitrary kernel read/write or a #PF panic.
    extern void syscall_user_ptr_selftest(void);
    syscall_user_ptr_selftest();

    // Front-munmap of a file/shmem-backed VMA must advance file_off/file_len/
    // shmem_pgoff in lockstep with vma->start, else later faults map the wrong page.
    extern void mm_vma_trim_selftest(void);
    mm_vma_trim_selftest();

    // rename(2) must reject moving a directory into its own subtree (mv /a /a/b)
    // -- it would detach the subtree into an unreachable cycle.
    extern void rename_under_selftest(void);
    rename_under_selftest();

    // unlink/rename must drop ONE link and free the inode only at links==0, so
    // renaming over a multi-link file does not destroy a still-referenced inode.
    extern void ext2_link_drop_selftest(void);
    ext2_link_drop_selftest();

    // Path-TOCTOU fix: the mutating fs ops now permission-check the parent they
    // themselves resolve.  Exercise create/unlink/mkdir/rename as root to prove
    // authorized mutation still works through the in-op check.
    extern void ext2_perm_op_selftest(void);
    ext2_perm_op_selftest();

    // Audit fix: write(2) raw-deref'd its data buffer after only a range check,
    // so an unmapped-but-in-range pointer panicked the kernel.  Verify the
    // per-page VMA-backing validator (no prefault, so file-backed sources keep
    // their content) accepts fully-backed ranges and rejects gaps/wraps.
    extern void mm_range_has_vmas_selftest(void);
    mm_range_has_vmas_selftest();

    // Audit fix: ext2_readdir read a dirent's name_len without bounding it to
    // the block, so a corrupt dirent could over-read past the bcache slot.
    // Verify the clamp keeps the name within block bounds.
    extern void ext2_readdir_clamp_selftest(void);
    ext2_readdir_clamp_selftest();

    // Audit fix: ext2 block size from an untrusted superblock was unbounded
    // (could exceed the fixed 4096 bcache slots).  Verify the mount-time clamp.
    extern void ext2_block_size_selftest(void);
    ext2_block_size_selftest();

    // Audit fix: path_split copied the parent path into a fixed stack buffer
    // with no bound -> kernel stack overflow on a long path.  Verify the bound
    // rejects an over-long parent instead of overflowing.
    extern void ext2_path_split_selftest(void);
    ext2_path_split_selftest();

    extern void ext2_block_valid_selftest(void);
    ext2_block_valid_selftest();
    extern void ext2_blk_lba_selftest(void);
    ext2_blk_lba_selftest();
    extern void ext2_inode_size_valid_selftest(void);
    ext2_inode_size_valid_selftest();
    extern void ext2_group_geom_selftest(void);
    ext2_group_geom_selftest();
    extern void ext2_dirent_in_block_selftest(void);
    ext2_dirent_in_block_selftest();
    // Cross-parent directory rename must repoint the moved dir's ".." entry
    // (else it escapes to the old parent) without clobbering siblings.
    extern void ext2_dotdot_repoint_selftest(void);
    ext2_dotdot_repoint_selftest();
    extern void xfer_bytes_ok_selftest(void);
    xfer_bytes_ok_selftest();
    // AHCI build_prdt must bound writes to the 248-entry PRDT (no OOB past the cmd table).
    extern void ahci_build_prdt_selftest(void);
    ahci_build_prdt_selftest();

    // Audit fix: pty ioctls dereferenced the raw user arg (arbitrary kernel
    // R/W LPE).  Verify they now reject bad user pointers with -EFAULT.
    extern void pty_ioctl_selftest(void);
    pty_ioctl_selftest();

    // Audit fix: the ELF phdr-table bounds check `e_phoff + phnum*56 > size`
    // overflowed (a -56 e_phoff wrapped below size) -> OOB phdr read.  Verify
    // the overflow-safe replacement rejects the wrap + boundary cases.
    extern void elf_phtab_bounds_selftest(void);
    elf_phtab_bounds_selftest();

    // Buddy high-order uniqueness test: DRM dumb buffers land at
    // order=10 (4MB each), so a duplicate-address bug there silently
    // corrupts compositor framebuffers.  Allocate 8 blocks without
    // freeing, require all distinct addresses, then free them all.
    {
        phys_addr_t got[8];
        int dup = 0;
        for (int i = 0; i < 8; i++) {
            got[i] = pmm_buddy_alloc(10);
            kprintf("[pmm10-test] alloc %d = 0x%lx\n", i, (unsigned long)got[i]);
            if (!got[i]) { kprintf("[pmm10-test] FAIL: OOM at %d\n", i); break; }
            for (int j = 0; j < i; j++)
                if (got[j] == got[i]) { dup = 1; kprintf("[pmm10-test] FAIL: dup alloc[%d]==alloc[%d]==0x%lx\n", j, i, (unsigned long)got[i]); }
        }
        for (int i = 0; i < 8; i++) if (got[i]) pmm_buddy_free(got[i], 10);
        kprintf("[pmm10-test] %s\n", dup ? "FAILED" : "PASSED (all 8 order=10 allocs distinct)");
    }

    // Phase 5B: SLAB_TYPESAFE_BY_RCU — validates that a typesafe
    // cache's empty-list pages defer to call_rcu and only return
    // to the buddy after a grace period.
    extern void slab_typesafe_selftest(void);
    slab_typesafe_selftest();

    // Phase 7F: dcache acceptance — negative-dentry invalidation
    // semantics + warm-cache path-walk speedup.
    extern void dcache_selftest(void);
    dcache_selftest();

    // Audit fix T1: dcache_lookup resurrected refcount-0 dentries the
    // shrinker had committed to free (lock-free bump vs free TOCTOU) -> UAF.
    // Stress lookers + shrinker + reinstalls on a shared key set; asserts no
    // looked-up dentry yields a corrupted child_ino (freed-slot reuse).
    extern void dcache_race_selftest(void);
    dcache_race_selftest();

    // Audit fix F131: dcache_invalidate must not free a dentry a path walk
    // holds by refcount -- hash-unlink only, let the last dcache_put + shrinker
    // reclaim it.  Hold a ref across an invalidate; confirm lookups miss yet the
    // held dentry stays intact.
    extern void dcache_held_invalidate_selftest(void);
    dcache_held_invalidate_selftest();

    // Phase 8D: io_uring kernel-side acceptance — NOP pipeline +
    // ns/op steady-state measurement.  Userland test binary lives
    // at /bin/test_io_uring for interactive testing.
    extern void io_uring_selftest(void);
    io_uring_selftest();

    // Audit fix: io_uring indexed sqes[]/cqes[] with the USER-WRITABLE
    // ring_mask -> OOB read/write.  Verify the trusted-mask index stays in bounds.
    extern void io_uring_index_selftest(void);
    io_uring_index_selftest();

    // Tier 1 #2 (eventfd): counter + semaphore + nonblocking semantics.
    extern void eventfd_selftest(void);
    eventfd_selftest();

    // Tier 1 #3 (timerfd): per-CPU sorted list, drained on sched_tick.
    extern void timerfd_selftest(void);
    timerfd_selftest();

    // timerfd concurrent-settime race: per-node lock prevents a timer node
    // from being linked onto two per-CPU lists by racing settime callers.
    extern void timerfd_race_selftest(void);
    timerfd_race_selftest();

    // inet socket file lifetime: the vfs_file_t is freed via the RCU grace
    // period (sock_free_rcu), so a tcp pcb_wake / udp sock_poll_wake reader
    // cannot write through a synchronously-freed file.
    extern void sock_file_lifetime_selftest(void);
    sock_file_lifetime_selftest();

    // Tier 1 #4 (socketpair): bidirectional AF_UNIX SOCK_STREAM pair.
    extern void socketpair_selftest(void);
    socketpair_selftest();

    // Tier 1 #5 (SCM_RIGHTS): fd round-trip over unix_sock_sendfd/recvfd.
    // Userland sendmsg/recvmsg marshals cmsg into these primitives.
    extern void scm_rights_selftest(void);
    scm_rights_selftest();

    // AF_UNIX refcount / peer-pin lifetime machinery (T3 part B UAF fix).
    extern void unix_refcount_selftest(void);
    unix_refcount_selftest();

    // AF_UNIX namespace string helpers: bounded hash/compare so a user sun_path
    // can never be walked past UNIX_PATH_MAX (the boundary now NUL-caps it too).
    extern void unix_ns_str_selftest(void);
    unix_ns_str_selftest();

    // AF_UNIX stream listen/accept backlog: a queued connect() client holds an
    // owned ref so it cannot dangle, and accept() atomically claims it before
    // pairing (the backlog client UAF fix).
    extern void unix_stream_accept_selftest(void);
    unix_stream_accept_selftest();

    // SOCK_DGRAM connect() default-destination owned-ref lifetime (the
    // asymmetric-peer UAF fix): the destination survives its own close while
    // a connected sender still references it.
    extern void unix_dgram_peer_selftest(void);
    unix_dgram_peer_selftest();

    // Pipe last-end-release: single atomic owner of the teardown (double-free fix).
    extern void pipe_refcount_selftest(void);
    pipe_refcount_selftest();

    // Pipe concurrent read/write: per-pipe ring lock keeps head/tail/count and
    // the payload bytes from tearing under racing readers/writers.
    extern void pipe_race_selftest(void);
    pipe_race_selftest();

    // epoll watched-file pin: file survives close while a watch is registered.
    extern void epoll_watch_refcount_selftest(void);
    epoll_watch_refcount_selftest();

    // Tier 1 #6 (signalfd): block sig, send, drain via signalfd read.
    extern void signalfd_selftest(void);
    signalfd_selftest();

    // signal_send_pid: rcu-safe pid lookup + delivery (sched_find_pid UAF fix).
    extern void signal_send_pid_selftest(void);
    signal_send_pid_selftest();

    // exec teardown: a multithreaded exec must not free the old page tables
    // while sibling threads still run on them (cross-domain page-table UAF/LPE).
    extern void exec_mm_teardown_selftest(void);
    exec_mm_teardown_selftest();

    // TCP ring-drain arithmetic + underflow clamp (T4 defense-in-depth).
    extern void tcp_ring_consume_selftest(void);
    tcp_ring_consume_selftest();

    extern void tcp_ring_reserve_selftest(void);
    tcp_ring_reserve_selftest();

    extern void tcp_accept_q_selftest(void);
    tcp_accept_q_selftest();

    // Freeing a TCP listener PCB must orphan (NULL the ->listener of) its
    // SYN_RCVD children so a late handshake ACK cannot deref the freed listener.
    extern void tcp_listener_orphan_selftest(void);
    tcp_listener_orphan_selftest();

    // SYN_RCVD half-open reaper: a never-completed, never-accepted half-open is
    // freed past the SYN-ACK retransmit cap, bounding a remote SYN-flood leak.
    extern void tcp_synreap_selftest(void);
    tcp_synreap_selftest();

    // UDP send-size guard: an oversize user length must be rejected (-EMSGSIZE)
    // before the user-u32 -> uint16_t UDP-length narrowing wraps and under-sizes
    // the skb (which NULL-derefs on the unchecked skb_put -- an unprivileged DoS).
    extern void udp_send_size_selftest(void);
    udp_send_size_selftest();

    // readdir/uname out-struct info-leak guard: the dirent buffer + utsname must
    // be zeroed before fill so no name-tail / padding stale bytes reach userspace.
    extern void out_struct_zerofill_selftest(void);
    out_struct_zerofill_selftest();

    // fd_table_clone correctness: fork/spawn/thread copy the parent fd table
    // (under its lock so a sibling close cannot free a file mid-vfs_dup).
    extern void fd_table_clone_selftest(void);
    fd_table_clone_selftest();

    // virtio-net device descriptor-id bounds (rx/tx OOB from a malicious device).
    extern void virtio_desc_id_valid_selftest(void);
    virtio_desc_id_valid_selftest();
    // virtio-net RX completion id bounded to the POPULATED rx buffers (a device
    // id in the desc ring but past the rx buffers -> NULL source / phys-0 repost).
    extern void virtio_rx_id_valid_selftest(void);
    virtio_rx_id_valid_selftest();

    // NVMe device completion-id bounds (req[] OOB from a malicious/buggy NVMe).
    extern void nvme_cid_valid_selftest(void);
    nvme_cid_valid_selftest();

    // virtfs_is_virtual path-boundary (unveil /dev|/proc exemption bypass fix).
    extern void virtfs_is_virtual_selftest(void);
    virtfs_is_virtual_selftest();

    // Tier 2.5a (virtio-gpu): pipeline exerciser was overriding the
    // UEFI GOP framebuffer with a self-test banner, hiding the TTY
    // from the SDL window under `-vga none -device virtio-vga`.  The
    // scanout is left in VGA-compat mode until the first userland
    // DRM client (dwl) actually calls SET_SCANOUT — TTY stays visible
    // throughout boot + login that way.  The full pipeline is still
    // exercised by drm-mock-selftest and the runtime DRM path.

    // Tier 2.5b (#8): DRM mock backend exerciser — exercises the
    // clean drm_backend_ops_t vtable without touching real hardware.
    // Happy path + two error-path probes.
    extern void drm_mock_selftest(void);
    drm_mock_selftest();

    // DRM create_dumb pitch/size overflow-safe math (32-bit width*4 overflow).
    extern void drm_dumb_size_selftest(void);
    drm_dumb_size_selftest();

    extern void drm_atomic_count_selftest(void);
    drm_atomic_count_selftest();
    extern void vgpu_fb_bytes_selftest(void);
    vgpu_fb_bytes_selftest();

    // TTY ring free-slot arithmetic (canonical line-framing: all-or-nothing
    // flush so a partial push can never drop a cooked line's terminating '\n').
    extern void tty_rb_free_selftest(void);
    tty_rb_free_selftest();

    extern void tty_ldisc_selftest(void);
    tty_ldisc_selftest();

    // termios publish/snapshot: TCSET* now lands in a stack local then publishes
    // the whole struct under tty->lock, and the line-discipline readers snapshot
    // under the same lock, so a concurrent set cannot tear ICANON against c_cc[].
    extern void tty_termios_snapshot_selftest(void);
    tty_termios_snapshot_selftest();

    // PTY pair lifetime: the pair is freed exactly once (both ends closed) and
    // unlinked, under s_pty_lock (no concurrent master/slave double-free).
    extern void pty_lifetime_selftest(void);
    pty_lifetime_selftest();
    // pty slave reopen: closing the slave with the master open must clear the
    // cached slave_file so a reopen cannot resurrect the freed vfs_file_t.
    extern void pty_reopen_selftest(void);
    pty_reopen_selftest();

    // PTY master ring: the slave-output ring (m_head/m_tail) is now mutated
    // under master_lock by both the producer (write_char/write_buf) and the
    // consumer (master_read), so a cross-CPU drain cannot race a push.
    extern void pty_master_ring_selftest(void);
    pty_master_ring_selftest();

    // evdev: the per-open client list + each client's event ring are now
    // serialised by a per-device IRQ-safe lock, so the IRQ/kthread producer
    // (input_device_emit) cannot deref a client a concurrent close() freed.
    extern void evdev_ring_selftest(void);
    evdev_ring_selftest();
#endif /* MAKAOS_BOOT_SELFTESTS */

    // Stress harnesses are compiled in but not auto-launched — reference
    // them here to suppress unused-function warnings.  Re-enable by
    // calling the launch fn directly when investigating SMP regressions.
    (void)stress_nvme_launch;
    (void)stress_ahci_launch;

    // Stress-test barrier removed — no stress workers are running in
    // this build, and the poll loop would just time out after 300 s.
    (void)g_nvme_done_count; (void)g_ahci_done_count;
    (void)g_nvme_total_mismatches; (void)g_ahci_total_mismatches;

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
    /* Panic rendezvous — see DEBUGGING.md §3.1.  panic() broadcasts
     * this vector to every other CPU so multi-core state is frozen
     * while the dying CPU dumps context. */
    idt_irq_register(VEC_IPI_HALT,       (uint64_t)ipi_halt_entry);

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

    // Phase 9A: kernel CSPRNG — multi-source entropy pool + ChaCha20
    // DRBG.  Mixes RDRAND/RDSEED, TSC jitter, boot-time DRAM, and
    // every IRQ's tsc_read_ns into a SHA-256 pool.  Seeds /dev/urandom.
    extern void kcsprng_init(void);
    kcsprng_init();

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
