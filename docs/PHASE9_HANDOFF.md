# Phase 9 SMP — Handoff Notes

This file is a self-note written before `/clear`.  Read this first on resume.
Authoritative plan is `docs/SMP_ARCHITECTURE.md`; this file captures
post-9-4 state + design decisions that haven't made it into that doc yet.

## Where we are

Branch: `test`.  Verify with `git log --oneline -10` — do not trust this
list once more than a few commits have landed after it.

Recent SMP commits (most-recent first):
- `9687842`  smp phase 9-4b/c — AP real-mode trampoline + `cpu_init_ap`
- `65247a9`  smp phase 9-4a  — per-CPU TSS array + shared GDT
- `45f0be0`  smp phase 9-3   — x2APIC + IPI machinery, online vs present cpu count
- `0b00a84`  smp phase 9-2   — ACPI MADT parser
- `b675076`  smp phase 9-1   — `%gs:offset` per-CPU accessors, `this_cpu()`

**All four CPUs boot online under `-smp 4`.**  Serial shows:

    [smp] bringing up APs, trampoline @ 0x8000
    [cpu_ap] online id=1 / 2 / 3
    [smp] done.  online=4  failed=0

BSP drives the full userland: login, bash, ps, makaterm, signals, DOOM.
The three APs spin in `cpu_init_ap` bumping `rcu_qs_count` every loop
iteration so `synchronize_rcu()` completes at line rate — no work yet,
but grace-period safe.  See "Why AP idle isn't `hlt`" below.

## What's done in Phase 9 so far

### 9-1 Per-CPU data (commit b675076)
- `cpu_t g_cpus[MAX_CPUS]` in `kernel/proc/cpu.c`, self-pointer at offset 0.
- GS_BASE MSR → `&g_cpus[id]`, so `mov %gs:0, %reg` is a single-instruction
  `this_cpu()`.
- `this_cpu_read_*` / `write_*` / `inc_*` macros in `kernel/include/cpu.h`
  encode field offsets as `%gs:%cN` displacements — one instruction per
  field access on the hot path (preempt_disable, current_task, etc.).

### 9-2 ACPI MADT (commit 0b00a84)
- `g_acpi.cpus[]` populated with every `enabled=1` processor entry from
  the MADT.  RSDP discovered via UEFI config table.  `g_acpi.cpu_count`
  is the hardware-present count (NOT the online count).

### 9-3 x2APIC + IPI plumbing (commit 45f0be0)
- `kernel/time/lapic.c` rewritten for x2APIC MSRs (0x800..0x83F).
- `lapic_send_init/sipi/ipi`, `lapic_init_ap`.
- `VEC_IPI_RESCHEDULE=0x40`, `VEC_IPI_TLB_FLUSH=0x41`, `VEC_IPI_CALL=0x42`
  reserved — **handlers not yet installed** (Phase 9-5).
- Introduced `g_num_cpus` (online) vs `g_acpi.cpu_count` (present) split.

### 9-4a Per-CPU TSS + shared GDT (commit 65247a9)
- `tss_t` embedded inline in `cpu_t`.  GDT grown to `6 + 2 * MAX_CPUS`
  slots; TSS descriptor for CPU N lives at `GDT[6 + 2*N]` with selector
  `GDT_TSS_SELECTOR(N) = (6 + 2*N) << 3`.  CPU 0 keeps 0x30 for
  bit-compat with the pre-SMP kernel.
- BSP pre-populates every CPU's TSS descriptor at boot (bases are BSS
  constants), so APs just `ltr` their slot — no GDT relocation.
- `syscall_entry.asm` loads RSP0 via `[gs:CPU_TSS_RSP0]`.  The offset is
  generated from C by a new `asm_offsets.c` → `asm_offsets.inc` build
  step (Linux-style).  Struct layout changes track automatically.
- New `tss_init_ap(uint32_t id)` that lgdts the kernel g_gdt, reloads
  segments, allocates IST stacks into `g_cpus[id].tss`, and LTRs the
  AP's own selector.

### 9-4b Real-mode trampoline (commit 9687842)
- `kernel/arch/x86_64/ap_trampoline.asm`, `nasm -f bin`, one 4 KiB page.
- Position-independent, three mode switches in one blob:
  real → protected (temp GDT, CR0.PE) → long (PAE, LME, CR0.PG) →
  absolute 64-bit jmp into `cpu_init_ap`.
- `build.sh` wraps the flat blob in an ELF object via
  `objcopy -I binary -O elf64-x86-64 -B i386:x86-64`, exposing
  `_binary_ap_trampoline_bin_{start,end,size}`.  Symbols resolve to
  `.rodata` inside the kernel image — `smp_boot.c` does a plain memcpy
  through HHDM to stage the blob in its reserved page.
- Reserved page: physical `0x8000` (SIPI vector `0x08`).  Inside the
  PMM's bottom-4-MiB reserved window, so the buddy allocator will never
  hand it out.  Identity-mapped via the loader's PML4[0] 1 GiB range.
- Startup state struct (`cr3`, `stack`, `entry`, `cpu_id`) at fixed
  offset `0x0F00` inside the page.  NASM asserts code stays below that.

### 9-4c BSP launcher + `cpu_init_ap` (commit 9687842)
- `kernel/arch/x86_64/smp_boot.c`:
  - `smp_boot_aps()` called from `init_kthread` after
    `do_initcalls_subsys()` and `ahci_start_io_thread()`.
  - Copies the trampoline into its page, walks `g_cpus[1..cpu_count)`,
    fires INIT → 10 ms → SIPI → 200 µs → SIPI, spins on `g_num_cpus`
    for 100 ms per AP.
  - Sanity: refuses to boot APs if kernel PML4 ≥ 4 GiB (trampoline
    loads CR3 from 32-bit protected mode).  Kernel PML4 always fits
    today because PMM services buddy allocations from low RAM in the
    platforms we target; the guard exists so a future high-memory
    allocator can't silently break AP boot.
- `cpu_init_ap(uint32_t id)` in `kernel/proc/cpu.c`:
  1. `tss_init_ap(id)` (which clobbers GS_BASE via its segment reload)
  2. `wrmsr(IA32_GS_BASE, &g_cpus[id])`
  3. `idt_load_ap()` (lidt the shared kernel IDT)
  4. `lapic_init_ap()` (x2APIC enable + LVT reset on this CPU)
  5. `atomic_fetch_add(&g_num_cpus, 1, RELEASE)` — BSP's spin-wait
     acquires this and returns success.
  6. **Infinite `pause` loop that increments `rcu_qs_count` each
     iteration.**  Intentionally NOT `hlt` — see next section.

## Why AP idle isn't `hlt` (critical)

We observed bash hanging immediately after `smp_boot_aps()` landed.
Root cause: `synchronize_rcu()` walks `[0, g_num_cpus)` and waits for
every CPU's `rcu_qs_count` to advance.  A halted AP with IRQs off never
advances its counter → infinite wait → deadlock on the first RCU writer
in the bash exec path (fdtable / VMA RCU).

Two lawful fixes:
1. Keep the AP halted but make `synchronize_rcu()` only consult
   *scheduling* CPUs.  Requires a separate "scheduling" bit per CPU.
2. Keep the AP actually advancing `rcu_qs_count`.

We took (2) for Phase 9-4 because it's self-contained and correct by
construction, with zero changes to RCU itself.  The tight `pause`
spinner bumps `rcu_qs_count` line-rate, so grace periods complete as
fast as the BSP can need them.

Phase 9-5 replaces this with a real idle task: scheduler tick fires on
the AP, the idle task runs, which bumps `rcu_qs_count` via the normal
context-switch path, and the AP can go back to `hlt` between ticks.

## Known limitations of the 9-4 state

- **APs do no work.**  Run queues, wake paths, `sched_add`, everything
  still targets `g_cpus[0].rq` only.  The BSP carries the whole load.
- **No IPI handlers installed.**  Vectors `0x40..0x42` are reserved but
  nothing fires or receives them.  `sched_wake()` cannot notify a
  target CPU that a new task is runnable — the target has to observe it
  at its next timer tick.  On APs there is no timer tick, so a task
  enqueued on an AP's run queue would never run.  This is why Phase 9-5
  must land in order (IPI first, then per-CPU timer + per-CPU sched).
- **No TLB shootdown.**  `munmap`/`mprotect` on one CPU do not flush
  the other CPUs' TLBs.  Single-process workloads are fine (processes
  never migrate yet), but any future cross-CPU mm sharing is broken
  until Phase 9-6 lands.
- **`rcu_qs_count` is a hot spinning write on every AP.**  Not a
  correctness issue (it's per-CPU, relaxed, no cache-line ping-pong)
  but it burns CPU time and wastes thermal budget.  Phase 9-5 fixes.

## Phase 9-5 — IPI handlers + AP timer + AP scheduling (NEXT)

Goal: APs run their own idle task on their own timer tick, and
`sched_wake()` across CPUs works.

### Concrete deliverables

1. **Install IPI handlers** (`kernel/arch/x86_64/idt.c` +
   `kernel/proc/sched.c`):
   - `VEC_IPI_RESCHEDULE` (0x40): handler EOIs and sets
     `this_cpu()->reschedule_pending`.  The real preemption happens on
     the next preempt_enable / return-to-userland check.
   - `VEC_IPI_CALL` (0x42): drains a per-CPU MPSC of
     `{fn, arg, done}` slots.  Sender does `__atomic_store(done=1)`
     under a completion flag the receiver checks.  Needed by TLB
     shootdown and by generic `smp_call_function_single`.
   - `VEC_IPI_TLB_FLUSH` (0x41): Phase 9-6 owns this — leave the
     vector reserved but wire an EOI-only stub now so a stray fire
     doesn't panic.

2. **Per-CPU idle task**.  `cpu_init_ap()` stops open-spinning and
   instead constructs an idle `task_t` (kind=kernel, entry=idle loop)
   and calls `sched_run_on_this_cpu(idle)`.  The idle loop does
   `sti; hlt` with the expectation that the LAPIC timer will wake it.

3. **Start the LAPIC timer on each AP**.  `cpu_init_ap()` calls
   `lapic_timer_start(SCHED_HZ)` after `lapic_init_ap()`.  BSP already
   calibrated `s_timer_ticks_per_ms` so APs reuse it.

4. **Wire `sched_wake()` to send a reschedule IPI**:
   - If target CPU is idle (`this_cpu()->current == this_cpu()->idle`)
     OR `preempt_depth == 0` and target is a different CPU → send
     `VEC_IPI_RESCHEDULE` to its `apic_id`.
   - If target CPU is the current CPU, just set `reschedule_pending`
     locally — no IPI, no wrmsr.

5. **RCU quiescent-state rewrite of the AP idle path**: remove the
   explicit `rcu_qs_count` bump in `cpu_init_ap`'s pre-scheduler spin.
   Once the idle task runs, every `hlt → IRQ → context switch back to
   idle` cycle naturally advances `rcu_qs_count` via the scheduler.

### Test plan

- `-smp 4`, boot to bash as today, verify:
  - `ps` shows idle tasks on CPUs 1/2/3.
  - Spawning 4 CPU-bound tasks causes them to land on four different
    CPUs (verify via `top`/per-task `cpu` field).
  - `synchronize_rcu()` still completes (no regression vs. 9-4).

### Commit message

`smp phase 9-5: IPI handlers + per-CPU LAPIC timer + AP idle tasks`

## Phase 9-6 — TLB shootdown

- Per-mm cpumask of CPUs that have loaded this mm (set in
  `sched_switch` when installing CR3).
- `vmm_page_unmap` / `vmm_page_flags_set` that retire user mappings
  iterate the cpumask and send `VEC_IPI_TLB_FLUSH` only to affected
  CPUs.  Payload: a per-CPU "shootdown slot" with a (start,end) range
  and a completion count, so batching works.
- Lazy TLB: kernel threads inherit the previous mm and skip the CR3
  reload entirely.  Saves the TLB flush on every kthread→user switch.

## Phase 9-7 — per-subsystem SMP soak + cleanups

- Multi-process DOOM on all 4 CPUs.
- `stress -c 4` burn.
- Signal delivery across CPUs.
- Remove any "Phase 9 TODO" leftovers in the scheduler.
- Tighten `g_num_cpus` semantics (should be an atomic from birth).

## Hard rules (user, repeated)

- **NO hacks, NO "simpler" approaches**.  Always the Linux-scale
  solution, never a TODO-left-behind.
- **Performance is #1**.  Hot paths O(1) / O(log n).  No redundant
  atomics.  No lock in any hot path that an end user can hit.
- Each phase = one commit.
- File reads go through `Bash` (never Read/Explore), and prefer
  `Grep` + `sed -n` to `cat`.

## Known traps / gotchas (retained)

- `mov %ax, %gs` (any segment load to %gs with ax==0) **clears
  GS_BASE**.  After any such load you must re-wrmsr IA32_GS_BASE.
  This is why `cpu_init_bsp` runs AFTER `tss_init` in kmain, and why
  `cpu_init_ap` does its GS_BASE wrmsr AFTER `tss_init_ap`.
- `g_num_cpus` = **online** CPUs.  `synchronize_rcu()` walks
  `[0, g_num_cpus)` and waits on each CPU's `rcu_qs_count`; a halted
  AP = infinite hang (see "Why AP idle isn't hlt" above).
- x2APIC ICR writes are atomic — no Delivery Status poll needed.
- MSI keeps 0xFEE00000 compat format; 8-bit dest field is fine for
  MAX_CPUS=64.
- Kernel PML4 must stay < 4 GiB until the AP trampoline adopts a
  64-bit CR3 load path.  `smp_boot_aps` asserts and refuses to boot
  otherwise.

## Deferred (do NOT do now)

- **Per-CPU slab magazines / Phase 4 redesign**: `docs/PHASE4_REDESIGN.md`.
  Lockless cmpxchg16b fast path + page-state shrinker.  Not until
  after phase 9 SMP is complete.
- **ext2 superblock free-counter writeback** — entry 14 in
  `SMP_KNOWN_RACES.md`.
- **ext2 per-CPU scratch slot** — entry 15 in `SMP_KNOWN_RACES.md`.

## On resume

1. `git status` + `git log --oneline -10` — confirm branch `test`,
   confirm 9-4b/c commit.  Build artifacts from the prior session may
   still be dirty; ignore them.
2. `grep -n "VEC_IPI_" kernel/time/lapic.h kernel/proc/sched.c` —
   find the current IPI vector reservations.
3. Read `cpu_init_ap` in `kernel/proc/cpu.c` and the AP idle loop
   comment — that is what Phase 9-5 will replace.
4. Begin Phase 9-5 with the IPI handler stubs in `idt.c`, then the
   handler bodies in `sched.c`, then the AP timer start, then the
   idle-task wiring.
