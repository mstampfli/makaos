# Phase 9 SMP — Handoff Notes

This file is a self-note written before `/clear`.  **Read this first
on resume.**  Authoritative plan is `docs/SMP_ARCHITECTURE.md`;
Phase 9-6 design is in `docs/PHASE9_6_PLAN.md`; historic race list
and resolutions are in `docs/SMP_KNOWN_RACES.md`.

## Where we are — RIGHT NOW

Branch: `test`.  Verify with `git log --oneline -10`.

Most recent SMP commits (latest first):

- `4782893`  docs: phase 9-6 plan — wait-queue audit, zero new locks
- `7b985f1`  **smp phase 9-5: SMP infrastructure + AP idle (pinned placement)**
- `d2f9327`  docs: refresh phase 9 status after 9-4b/c
- `9687842`  smp phase 9-4b/c — AP real-mode trampoline + `cpu_init_ap`
- `65247a9`  smp phase 9-4a  — per-CPU TSS array + shared GDT
- `45f0be0`  smp phase 9-3   — x2APIC + IPI machinery, online vs present cpu count
- `0b00a84`  smp phase 9-2   — ACPI MADT parser
- `b675076`  smp phase 9-1   — per-CPU `this_cpu()`

**Phase 9-5 is DONE as "SMP infrastructure + AP idle".**  APs are
fully online, running a proper per-CPU idle task on their own kstack,
handling IPIs, and advancing their own RCU quiescent state through
`sched_tick`.  Serial boot shows:

    [smp] bringing up APs, trampoline @ 0x8000
    [cpu_ap] online id=1 / 2 / 3
    [smp] done.  online=4  failed=0

BSP drives the full userland: login → bash → ps → makaterm → DOOM →
http_get, exactly as before.

**Phase 9-5's explicit scope cut:** user-task PLACEMENT is pinned
to CPU 0 via `pick_home_cpu() = 0`.  APs do NO user work yet.  The
cross-CPU infrastructure is live (sched_wake, wake_pending, IPIs,
per-CPU RCU qs) but it's not yet exercised by `sched_add` /
`sched_wake` from a real user-task path.  That's deliberate — see
"Why pinned" below and the Phase 9-6 plan.

## Phase 9 progress (what's committed)

### 9-1 Per-CPU data (commit `b675076`)
- `cpu_t g_cpus[MAX_CPUS]` with self-pointer at offset 0.
- GS_BASE MSR → `&g_cpus[id]`, one-insn `this_cpu()`.
- `this_cpu_read_*` / `write_*` / `inc_*` macros encode field
  offsets as `%gs:%cN` displacements.

### 9-2 ACPI MADT (commit `0b00a84`)
- `g_acpi.cpus[]` populated from MADT; `g_acpi.cpu_count` is the
  hardware-present count (NOT the online count).

### 9-3 x2APIC + IPI plumbing (commit `45f0be0`)
- `kernel/time/lapic.c` rewritten for x2APIC MSRs.
- `lapic_send_init/sipi/ipi`, `lapic_init_ap`.
- IPI vectors reserved: `VEC_IPI_RESCHEDULE=0x40`,
  `VEC_IPI_TLB_FLUSH=0x41`, `VEC_IPI_CALL=0x42`.
- `g_num_cpus` (online) vs `g_acpi.cpu_count` (present) split.

### 9-4a Per-CPU TSS + shared GDT (commit `65247a9`)
- `tss_t` embedded inline in `cpu_t`.
- GDT sized `6 + 2 * MAX_CPUS` slots; CPU N's TSS descriptor at
  `GDT[6 + 2*N]` with selector `(6 + 2*N) << 3` (CPU 0 keeps 0x30).
- `syscall_entry.asm` loads RSP0 via `[gs:CPU_TSS_RSP0]`; the offset
  is generated from C via `asm_offsets.c` → `asm_offsets.inc`.

### 9-4b Real-mode trampoline (commit `9687842`)
- `kernel/arch/x86_64/ap_trampoline.asm`, `nasm -f bin`, one 4 KiB
  page.  Real → protected → long in one blob.
- `build.sh` wraps it in an ELF object via
  `objcopy -I binary -O elf64-x86-64` so the linker places it in
  `.rodata` with `_binary_ap_trampoline_bin_{start,end,size}`
  symbols.
- Trampoline phys page: `0x8000`, SIPI vector `0x08`, inside the
  PMM's bottom-4-MiB reserved window.

### 9-4c BSP launcher + `cpu_init_ap` (commit `9687842`, same as 4b)
- `smp_boot_aps()` copies the blob into its page, runs
  INIT → 10 ms → SIPI → 200 µs → SIPI, spins on `g_num_cpus`.
- `cpu_init_ap(id)` runs from the trampoline's final jump and
  does: `tss_init_ap` → wrmsr GS_BASE → `idt_load_ap` →
  `lapic_init_ap` → announce online.
- Historical: initially ended in a `pause`-spin that bumped
  `rcu_qs_count`.  Phase 9-5 replaced this with a proper per-CPU
  idle task (see below).

### 9-5 SMP infrastructure + AP idle (commit `7b985f1`) — **LATEST**

Real per-CPU bring-up in `cpu_init_ap` (mirrors kmain's early code):
- IA32_PAT (WC entry for the framebuffer)
- CR0.{EM,TS} cleared, CR4.{OSFXSR, OSXMMEXCPT, SMEP} set
- `syscall_init()` — IA32_EFER.SCE, STAR, LSTAR, SFMASK
- then TSS / GS / IDT / LAPIC (as in 9-4c)

Per-CPU idle task:
- `sched_init_idle_for_cpu(id)` kmallocs a `task_t`, allocates a
  dedicated kstack via `kstack_alloc()`, captures a valid FPU
  baseline with `fxsave`, and wires `proc_trampoline → cpu_idle_loop`
  onto its initial stack.
- The idle task's `mm_shared` points at the kernel PML4 (NOT 0),
  so `context_switch` into idle reloads CR3 off of whatever user
  mm was previously installed — this closes the "dead-PML4 in CR3
  after user task exits" class of faults.
- `cpu_init_ap` hands off to `sched_enter_idle()` which calls
  `cpu_idle_loop` directly (bypassing `sched_yield`, which would
  identity-context-switch-to-self and return straight back to the
  AP init frame — see commit message for the full trace).

`do_switch` correctness fixes:
- Empty-rq path now switches to the per-CPU idle task instead of
  inline-hlt'ing on the outgoing task's kstack.  The old inline
  hlt is a kstack UAF with zombie reap: a zombie spinning in
  do_switch's own hlt while another CPU reaps it and kstack_free's
  the same memory.
- The preempted + empty-rq + running-cur early-return path now
  calls `rcu_note_qs()`.  Without it, a CPU parked in a
  non-yielding busy loop (init_kthread's `proc_trampoline.dead`
  spin) never advances its RCU qs counter, and the next
  `synchronize_rcu()` from any other CPU hangs forever.

`synchronize_rcu` correctness:
- Cross-CPU wait loop now uses `cpu_relax()` instead of
  `sched_yield()`.  `sched_yield` on an otherwise-idle CPU parks
  forever inside `do_switch`'s hlt loop and never returns, making
  the outer `while (counter == snap)` loop unreachable.

`sched_wake` + `wake_pending` (lost-wakeup protection):
- New `task_t.wake_pending` u8 field.  Owned by the task's home
  rq_lock.
- `sched_wake` takes the lock and, if `target->state !=
  TASK_SLEEPING`, sets `wake_pending=1` instead of dropping the
  wake.
- `sched_sleep` checks `wake_pending` under the same lock BEFORE
  committing to sleep and bails out if set.
- `signal_send` now unconditionally calls `sched_wake` (no racy
  "if state == TASK_SLEEPING" skip).  `signal_deliver_pending`'s
  fatal-signal death path also signal_sends SIGCHLD to the parent.

`fb_term_putc` serialization:
- New `g_fb_lock` IRQ-safe spinlock replaces `preempt_disable` on
  `fb_term_putc` / `fb_term_write`.  preempt_disable only blocks
  LOCAL task switching; under SMP two CPUs in `fb_term_putc` raced
  on `g_fb_col/row`, pixel memory, and the scroll memcpy.
- Documented in `docs/LOCKS.md`.

IPI handlers wired (in `kernel/arch/x86_64/ipi.c`):
- `VEC_IPI_RESCHEDULE`: sets `reschedule_pending=1` and returns.
  Does NOT call `sched_preempt()` — a nested `do_switch(1)` from
  inside an IPI that fired during the outer do_switch's hlt loop
  dequeues the freshly-enqueued task, identity-context-switches
  back through the IPI stack, and leaves the outer loop to hlt
  again on the now-empty rq.  The correct path is: IPI wakes
  hlt, iretq returns into `cli`, the outer loop re-locks,
  dequeues the task, proceeds naturally.
- `VEC_IPI_CALL`: drains a per-CPU MPSC queue of
  `{fn, arg, done}` slots.  Skeleton only — no callers yet,
  Phase 9-7's TLB shootdown is the first user.
- `VEC_IPI_TLB_FLUSH`: EOI-only no-op until 9-7.

**Why pinned (the explicit 9-5 scope cut):**
- Every cross-CPU wait-queue subsystem (pty, epoll, waitpid, pipe,
  unix socket, AHCI rendezvous) has its own idiomatic lost-wakeup
  pattern that's harmless on UP but triggers the moment a
  round-robin `pick_home_cpu` puts a writer on one CPU and a reader
  on another.
- We fixed the scheduler primitive (`wake_pending` + unconditional
  `sched_wake` from `signal_send`) but NOT the per-subsystem use
  sites.  The use-site audit is Phase 9-6.
- Placement pins to CPU 0 via `pick_home_cpu() = 0` until 9-6
  lands.

## What works today

- `-smp 4` boot, 4 CPUs online
- Full userland on BSP (login, bash, ps, makaterm, DOOM, net,
  http_get, etc.) — no functional regression vs. 9-4b/c
- APs idle correctly, handle reschedule IPIs (exercised via
  `signal_send` cross-CPU — all sends come from CPU 0 today)
- RCU grace periods complete cross-CPU
- No kstack UAFs, no PML4 UAFs, no FPU corruption, no SIGILL on
  AP scheduling

## What does NOT work yet

- **Round-robin task placement.**  `pick_home_cpu()` always
  returns 0.  Flipping it to round-robin today causes intermittent
  hangs in pty / epoll / waitpid (see SMP_KNOWN_RACES entry 24).
- **TLB shootdown.**  `munmap`/`mprotect` on one CPU do not flush
  remote TLBs.  Fine while placement is pinned because there's
  only one CPU touching user memory.  Phase 9-7.
- **Load balancing / work stealing.**  Phase 9-8.

## Phase 9-6 — NEXT

Authoritative plan: `docs/PHASE9_6_PLAN.md`.  Summary:

> Audit every sleep/wake use site to match the canonical pattern
> (phase 1 check → phase 2 register → phase 3 re-check → phase 4
> sleep).  Zero new locks expected.  Flip `pick_home_cpu()` back
> to round-robin as the last step.

Subsystems, in order:
- 9-6a `sys_wait` children walk
- 9-6b pty slave/master + drain wait queues
- 9-6c epoll_wait `has_ready` flag ordering
- 9-6d pipe reader/writer
- 9-6e unix socket both halves
- 9-6f AHCI submitter/io_thread rendezvous
- 9-6g flip `pick_home_cpu` → round-robin

Each step is one commit with a concrete reproducer in
`userland/apps/smp_test/smp_test.sh` (doesn't exist yet — write it
first as its own commit).

## GDB workflow (preserved from 9-5 debugging)

This saved us many hours in 9-5.  Keep it.

### Start QEMU paused in the background with GDB attached

```bash
# From the repo root, after ./build.sh has built disk.img:

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="build/OVMF_VARS.fd"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS"

qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 256M \
  -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file="build/disk.img",if=none,id=hd0 \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga std -display sdl \
  -audiodev pa,id=snd0,server=/run/user/1000/pulse/native \
  -device intel-hda -device hda-duplex,audiodev=snd0 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -serial file:build/serial.txt \
  -monitor none -gdb tcp::1234 \
  -no-reboot -no-shutdown &
```

Drop `-display sdl` and the audio/net lines for a headless run.
Add `-S` to start paused.  Without `-S`, the kernel boots and you
attach a running gdb when a hang is reproduced.

### Attach gdb to a running qemu and dump all 4 CPUs

```bash
cat > /tmp/gdb_dump.txt <<'EOF'
file build/kernel.elf
target remote :1234
set confirm off
set pagination off

interrupt

info threads

set $cpu = 0
while $cpu < 4
  printf "\n===== CPU%d =====\n", $cpu
  thread_id = $cpu + 1
  # bt for each CPU
  # print current pid/comm/state
end

# per-CPU current
p g_cpus[0].current->pid
p g_cpus[0].current->comm
p g_cpus[0].current->state
p g_cpus[0].reschedule_pending
# (repeat 0..3)

# walk sleep + rq lists per CPU
set $cpu = 0
while $cpu < 4
  printf "-- CPU%d --\n", $cpu
  set $i = 0
  while $i < 4
    set $t = g_cpus[$cpu].rq.heads[$i]
    while $t != 0
      printf "  rq[%d] pid=%d st=%d wp=%d comm=%s\n", $i, $t->pid, $t->state, $t->wake_pending, $t->comm
      set $t = $t->next
    end
    set $i = $i + 1
  end
  set $t = g_cpus[$cpu].rq.sleep_head
  while $t != 0
    printf "  sleep pid=%d st=%d wp=%d comm=%s ppid=%d\n", $t->pid, $t->state, $t->wake_pending, $t->comm, $t->ppid
    set $t = $t->next
  end
  set $t = g_cpus[$cpu].rq.zombie_head
  while $t != 0
    printf "  zomb pid=%d st=%d ppid=%d comm=%s\n", $t->pid, $t->state, $t->ppid, $t->comm
    set $t = $t->next
  end
  set $cpu = $cpu + 1
end

# enumerate all known tasks via the pid_ht
set $i = 0
while $i < s_pid_ht->cap
  set $s = s_pid_ht->slots[$i]
  if $s != 0 && $s != (task_t*)1
    if $s->pid > 10
      printf "  pid=%d state=%d wp=%d home=%d comm=%s\n", $s->pid, $s->state, $s->wake_pending, $s->home_cpu, $s->comm
    end
  end
  set $i = $i + 1
end

# RCU QS counters — a CPU whose counter is frozen is the first
# thing to look at when synchronize_rcu hangs
p g_cpus[0].rcu_qs_count
p g_cpus[1].rcu_qs_count
p g_cpus[2].rcu_qs_count
p g_cpus[3].rcu_qs_count

# Look up a specific task by pid
# set $b = pid_ht_find(18)
# p *$b
# x/32gx $b->ctx.rsp       -- dump its saved stack
EOF
timeout 15 gdb -batch -x /tmp/gdb_dump.txt 2>&1 | tail -200
```

### Resolve stack addresses to function names

```bash
# Grab code-range-looking qwords from a saved stack
# Range: 0xffffffff80000000 – 0xffffffff80100000 (kernel .text)

(gdb) info symbol 0xffffffff8000c6ec
do_switch + 508 in section .text

# Or from shell:
$ addr2line -e build/kernel.elf -f 0xffffffff8000c6ec
```

### Known task-struct corruption symptoms

- If `comm` is machine code and `sigstate.handlers[]` is garbage,
  the task_t was allocated from a slab slot whose previous contents
  weren't cleared by `task_init_common`.  It's NOT memory
  corruption — see entry #21 in SMP_KNOWN_RACES for the real
  issue.  Don't chase the wrong bug.  The pid/state/ctx/
  mm_shared/files_shared fields ARE the source of truth.

- If `current->pid` in a CPU's cpu_t doesn't match a task whose
  `kstack_top` covers the CPU's actual rsp, the CPU is MID
  `context_switch` — the store to `c->current = next` has landed
  but the asm is still running the pops/fxrstor on the outgoing
  task's stack.  Wait a few µs and re-interrupt.

### Don'ts (hard-won)

1. **Don't add serial debug logs to diagnose races.**  Serial is
   unbuffered and slow (~10 kB/s).  Every `serial_puts_dbg` adds
   microseconds to the code path and changes timing enough to
   move or hide the race.  Use GDB snapshots instead.

2. **Don't call `sched_preempt()` from an IPI handler.**  A nested
   `do_switch(1)` from inside an IPI that fired during an outer
   `do_switch`'s hlt loop dequeues the freshly-enqueued task,
   identity-context-switches back through the IPI stack, and
   leaves the outer loop to hlt forever on the now-empty rq.
   The IPI handler sets `reschedule_pending=1` and returns; the
   outer hlt wakes, re-locks, re-dequeues, proceeds naturally.

3. **Don't use `sched_yield()` inside a spin loop in
   `synchronize_rcu` or similar.**  Yield on an otherwise-idle
   CPU parks forever inside `do_switch`'s hlt loop and never
   returns.  Use `cpu_relax()`.

4. **Don't let `do_switch` inline-hlt on the outgoing task's
   kstack.**  Zombie on CPU A in do_switch's hlt loop while CPU B
   reaps it and `kstack_free`s the same memory = kstack UAF.
   `do_switch` with empty rq MUST switch to the per-CPU idle
   task's dedicated kstack.

5. **Don't assume `signal_send` fires on SIGCHLD.**  The old
   `if (state == TASK_SLEEPING) sched_wake(parent)` fast path
   was a lost-wakeup race.  Use the Phase 9-5 pattern:
   `signal_send(parent, SIGCHLD)` unconditionally; `signal_send`
   always calls `sched_wake`, which correctly handles every
   state under the home's rq_lock.

6. **Don't hlt an AP without a way to tick.**  An AP that
   `sti; hlt`s with IRQs off or without a periodic timer never
   advances its `rcu_qs_count`; the next `synchronize_rcu()` on
   any other CPU hangs forever.  The Phase 9-5 idle task runs
   `rcu_note_qs()` + `sti; hlt` + `sched_yield()` on a periodic
   timer, which bumps the counter on every loop.

## Hard rules (user, repeated many times)

- **NO hacks, NO "for now" approaches.**  Always the
  Linux-scale correct solution.  No TODOs-left-behind.
- **Performance is #1.**  Hot paths O(1)/O(log n).  NO new
  locks on any hot path.  NO new atomics or MSR writes on the
  hot path unless measurement justifies them.
- Each phase = one commit.
- **File reads via `Bash`**, never `Read`/`Explore` — kernel files
  are too large and break those tools.  Prefer `Grep` + `sed -n`
  ranges over `cat`.

## Deferred (do NOT do now)

- **Per-CPU slab magazines / Phase 4 redesign** —
  `docs/PHASE4_REDESIGN.md`.  Not until after phase 9 is fully
  complete.
- **ext2 superblock free-counter writeback** — entry 14 in
  `SMP_KNOWN_RACES.md`.
- **ext2 per-CPU scratch slot** — entry 15 in `SMP_KNOWN_RACES.md`.

## On resume

1. `git status` + `git log --oneline -10` — confirm branch `test`,
   confirm `7b985f1` (9-5) is the latest SMP commit.
2. Read `docs/PHASE9_6_PLAN.md` — it's the plan for the next phase.
3. Read this file's "GDB workflow" and "Don'ts" sections BEFORE
   touching any cross-CPU code.
4. Phase 9-6 is a series of small commits, one per subsystem.  The
   first commit is the `smp_test.sh` reproducer itself.
5. `pick_home_cpu()` stays returning 0 until every subsystem in the
   9-6 plan is audited + its reproducer passes.  Then a final
   single-line commit flips it to round-robin.
