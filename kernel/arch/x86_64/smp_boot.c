// SMP application-processor bring-up (phase 9-4b/c).
//
// The BSP, running in init_kthread, reserves a low-memory physical page,
// copies the ap_trampoline.bin blob into it, and uses INIT/SIPI to walk
// every non-BSP CPU into long mode where `cpu_init_ap` takes over.
//
// Bring-up is serialised: one AP at a time shares the same trampoline
// page and the same startup_state struct inside it.  Once an AP has
// advanced past reading startup_state it is safe for the BSP to reuse
// the struct for the next AP.  We conservatively wait for the AP's
// g_num_cpus contribution (post-tss/idt/gs init, well after the final
// jmp out of the trampoline page) before moving to the next — that way
// a slow AP can't race the BSP rewriting its startup record.
//
// Startup order: BSP first (already running), then APs in the order
// ACPI listed them.  Fixing the order now makes cpu_id assignment
// stable across boots, which the scheduler and pin-to-cpu syscalls
// both benefit from.

#include "common.h"
#include "smp_boot.h"
#include "acpi.h"
#include "lapic.h"
#include "cpu.h"
#include "tss.h"
#include "idt.h"
#include "vmm.h"
#include "pmm.h"
#include "tsc.h"

// ── Symbols exported by the trampoline object (objcopy -I binary) ───────
// build.sh runs `objcopy -I binary -O elf64-x86-64 -B i386:x86-64
// ap_trampoline.bin ap_trampoline.o`, which synthesises three symbols:
//
//   _binary_ap_trampoline_bin_start  — byte 0 of the blob
//   _binary_ap_trampoline_bin_end    — one past the last byte
//   _binary_ap_trampoline_bin_size   — size as an absolute value
//
// Declared as `char[]` so taking their address yields a linker-resolved
// virtual pointer into the kernel image.
extern char _binary_ap_trampoline_bin_start[];
extern char _binary_ap_trampoline_bin_end[];

// Fixed startup_state offset — must match STARTUP_OFF in ap_trampoline.asm.
// Mirrored here as a plain constant so the C side doesn't need to parse
// another asm_offsets header.  A static_assert would be nice but there is
// nothing from the asm side to compare against at compile time; the
// guarantee comes from the asm file's own `%if` guard.
#define AP_STARTUP_OFF   0x0F00

typedef struct __attribute__((packed)) {
    uint64_t cr3;       // kernel PML4 physical  (MUST be < 4 GiB — see comment)
    uint64_t stack;     // 64-bit RSP for cpu_init_ap on this AP
    uint64_t entry;     // address of cpu_init_ap (higher-half)
    uint32_t cpu_id;    // index into g_cpus[]
    uint32_t _pad;
} ap_startup_state_t;

// ── Trampoline page ─────────────────────────────────────────────────────
// A single physical page below 1 MiB.  We pick 0x8000 (SIPI vector 0x08)
// because it's inside the PMM's bottom-4-MiB reserved window (so the
// buddy allocator will never hand it out), sits above BIOS data
// (0x0000..0x04FF), above IVT/BDA, and well below typical EBDA (0x9Fxxx).
// Every major OS reserves a page in this range for the same purpose.
#define AP_TRAMPOLINE_PHYS  0x8000ULL

// HHDM view of the trampoline page — all of kernel's lower 4 MiB is
// reachable via HHDM_OFFSET once vmm_init has run.
static inline volatile uint8_t* tramp_hhdm(void) {
    return (volatile uint8_t*)(AP_TRAMPOLINE_PHYS + HHDM_OFFSET);
}
static inline volatile ap_startup_state_t* tramp_ss(void) {
    return (volatile ap_startup_state_t*)(tramp_hhdm() + AP_STARTUP_OFF);
}

// ── Timing helpers built on tsc_read_ns ─────────────────────────────────
// We only need microsecond- and millisecond-scale delays during INIT/SIPI
// and the AP-up spin — not worth introducing a full udelay() subsystem
// for this one file.  tsc_read_ns is monotonic post-calibration.
static inline void busy_wait_us(uint64_t us) {
    uint64_t deadline = tsc_read_ns() + us * 1000ull;
    while (tsc_read_ns() < deadline) cpu_relax();
}
static inline void busy_wait_ms(uint64_t ms) {
    busy_wait_us(ms * 1000ull);
}

// ── AP bring-up inner loop ──────────────────────────────────────────────
// Returns 1 if the AP came online, 0 on timeout.  `target_id` is the
// g_cpus[] slot the AP will claim; `target_apic_id` is its hardware
// LAPIC ID (from ACPI).
//
// We spin on g_num_cpus rather than on a per-AP flag because
// cpu_init_ap()'s atomic_fetch_add(&g_num_cpus) is the canonical signal
// that the AP has finished all its init (tss, idt, gs, lapic) — any
// earlier signal would be a lie.
static int bring_up_one_ap(uint32_t target_id, uint32_t target_apic_id,
                            uint64_t kpml4, uint64_t entry_fn) {
    // Each AP gets its own 16 KiB kernel stack — used from the moment
    // lm_entry does `mov rsp, [startup.stack]`, through cpu_init_ap, and
    // for the rest of the AP's life (until the scheduler switches it
    // onto per-task kstacks).  Guard-paged by kstack_alloc.
    virt_addr_t ap_stack_top = kstack_alloc();

    // Publish the per-AP startup record to the trampoline page.
    volatile ap_startup_state_t* ss = tramp_ss();
    ss->cr3    = kpml4;
    ss->stack  = (uint64_t)ap_stack_top;
    ss->entry  = entry_fn;
    ss->cpu_id = target_id;
    ss->_pad   = 0;

    // A full store barrier so the trampoline-side loads after the SIPI
    // see the fully-written record.  x86 is already TSO for store→store
    // but we're writing through HHDM and reading through a physical
    // alias on the AP — be explicit.
    smp_mb();

    uint32_t baseline = atomic_load(&g_num_cpus);

    // Classic INIT/SIPI/SIPI sequence:
    //   1. INIT IPI  — drops target into INIT state
    //   2. 10 ms     — spec-mandated "INIT delay"
    //   3. SIPI #1   — vector 0x08 (trampoline_phys >> 12)
    //   4. 200 µs    — spec-mandated settle time before retry
    //   5. SIPI #2   — safety retry; if AP already started from #1 it is
    //                  a no-op (SIPI is ignored while the CPU is running
    //                  outside the wait-for-SIPI state).
    lapic_send_init(target_apic_id);
    busy_wait_ms(10);

    uint8_t sipi_vector = (uint8_t)(AP_TRAMPOLINE_PHYS >> 12);
    lapic_send_sipi(target_apic_id, sipi_vector);
    busy_wait_us(200);
    lapic_send_sipi(target_apic_id, sipi_vector);

    // Wait up to 100 ms for the AP to complete cpu_init_ap and bump
    // g_num_cpus.  On real hardware this typically completes in <1 ms;
    // 100 ms gives QEMU a comfortable margin under CI load.
    uint64_t deadline = tsc_read_ns() + 100ull * 1000ull * 1000ull;
    while (tsc_read_ns() < deadline) {
        if (atomic_load(&g_num_cpus) > baseline) return 1;
        cpu_relax();
    }
    return 0;
}

// ── Entry: smp_boot_aps ─────────────────────────────────────────────────
void smp_boot_aps(void) {
    if (1) { serial_puts_dbg("[smp] FORCED-UNIPROCESSOR (debug)\n"); return; }
    if (!g_acpi.ok || g_acpi.cpu_count <= 1) {
        serial_puts_dbg("[smp] uniprocessor — skipping AP bring-up\n");
        return;
    }

    // ── Sanity: kernel PML4 must fit in 32 bits ─────────────────────────
    // The trampoline loads CR3 from 32-bit protected mode via
    // `mov cr3, eax`, which clears bits [63:32] of CR3 on the transition
    // to long mode.  Every PML4 the kernel builds today comes from the
    // low buddy pool (well under 4 GiB), so this is always satisfied in
    // practice — enforce it here so a future change that allocates PML4
    // pages from high memory gets caught the moment it tries to SMP boot.
    phys_addr_t kpml4 = vmm_kernel_pml4_get();
    if (kpml4 >> 32) {
        serial_puts_dbg("[smp] FATAL: kernel PML4 above 4 GiB — refusing to boot APs\n");
        return;
    }

    // ── Copy the trampoline into its reserved low-memory page ──────────
    const uint8_t* src     = (const uint8_t*)_binary_ap_trampoline_bin_start;
    const uint8_t* src_end = (const uint8_t*)_binary_ap_trampoline_bin_end;
    uint64_t blob_size = (uint64_t)(src_end - src);
    if (blob_size > PAGE_SIZE) {
        serial_puts_dbg("[smp] FATAL: trampoline blob larger than one page\n");
        return;
    }

    volatile uint8_t* dst = tramp_hhdm();
    for (uint64_t i = 0; i < blob_size; i++) dst[i] = src[i];
    // Zero the tail (scratch space beyond STARTUP_OFF is re-used each AP).
    for (uint64_t i = blob_size; i < PAGE_SIZE; i++) dst[i] = 0;

    // The trampoline runs off physical memory accessed with CS base =
    // AP_TRAMPOLINE_PHYS.  After it enters long mode it uses kernel CR3,
    // which identity-maps the low 1 MiB via PML4[0] (set up by the
    // loader).  Nothing further is needed to make the page "reachable"
    // from the APs — they come out of reset already seeing low phys.

    uint32_t brought_up = 0;
    uint32_t failed     = 0;

    serial_puts_dbg("[smp] bringing up APs, trampoline @ 0x");
    serial_hex_dbg(AP_TRAMPOLINE_PHYS);

    // g_cpus[0] is the BSP; walk the rest of the discovered CPUs.  Slot
    // indices match ACPI-discovery order (cpu_init_bsp made that stable).
    for (uint32_t id = 1; id < g_acpi.cpu_count && id < MAX_CPUS; id++) {
        uint32_t apic_id = g_cpus[id].apic_id;

        serial_puts_dbg("[smp] AP id=");
        serial_hex_dbg((uint64_t)id);
        serial_puts_dbg("[smp]   apic=");
        serial_hex_dbg((uint64_t)apic_id);

        int ok = bring_up_one_ap(id, apic_id, kpml4, (uint64_t)&cpu_init_ap);
        if (ok) {
            brought_up++;
            serial_puts_dbg("[smp]   online\n");
        } else {
            failed++;
            serial_puts_dbg("[smp]   TIMEOUT — left halted\n");
        }
    }

    serial_puts_dbg("[smp] done.  online=");
    serial_hex_dbg((uint64_t)atomic_load(&g_num_cpus));
    serial_puts_dbg("[smp] failed=");
    serial_hex_dbg((uint64_t)failed);
    (void)brought_up;
}
