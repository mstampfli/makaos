# Debugging

How to diagnose bugs in MakaOS and systems-level C/NASM code.

**Strong defaults.** Deviations require justification.

Companion to `CODE_STYLE.md` (how code looks), `PRINCIPLES.md` (how
code is built), and `MAKAOS.md` (project-specific context).

---

## 1. The workflow

**Read serial first. Then attach GDB. Never only serial.**

Serial tells you *what happened*. GDB tells you *why*. Skipping
either step means you're debugging with half the information.

The canonical loop:

1. **Reproduce.** If you can't reproduce it, you can't fix it.
   Minimize the repro before anything else.
2. **Read the serial log.** The panic output, the last `pr_info`
   before the crash, the subsystem tag of the last message — these
   narrow the search space by orders of magnitude.
3. **Form a hypothesis** from the log. What was the kernel doing?
   What subsystem? What state?
4. **Attach GDB.** Confirm or refute the hypothesis. Look at
   registers, the stack, the data structures.
5. **Fix the root cause.** Not the symptom. See `PRINCIPLES.md §2` —
   no hacks.
6. **Add a test** (self-test or integration) that would have caught
   it. The bug escaped the existing tests; the tests need to grow.

### Anti-patterns

- Jumping straight to GDB without reading the serial log. You will
  miss context the log already gave you.
- Debugging without a hypothesis. "Let me step through and see what
  happens" is a waste of time; form a theory first and test it.
- Fixing the crash site without understanding why the invariant was
  violated upstream.
- "It works now, I don't know why" — the bug is still there.

---

## 2. Serial logging

### 2.1 Format

Every log line carries:

```
[    12.345678] [CPU0] [sched:INFO] picking next task: pid=42
 ^timestamp     ^cpu    ^subsystem
                        ^level
```

- **Timestamp:** seconds since boot, microsecond precision, fixed
  width. Fixed width matters — misaligned timestamps are hard to
  scan visually.
- **CPU id:** `CPU0`, `CPU1`, etc. Non-optional on SMP.
- **Subsystem tag:** the subsystem emitting the log (`sched`, `ext2`,
  `pipe`, `svcmgr`, `vm`). Matches the function prefix convention
  from `CODE_STYLE.md §2`.
- **Level:** `DEBUG`, `INFO`, `WARN`, `ERR`, `CRIT`. Uppercase.

### 2.2 Levels

- **`DEBUG`** — verbose, development only. Compiled out in release.
- **`INFO`** — normal operational events. Subsystem init, service
  startup, task spawn/exit.
- **`WARN`** — unexpected but recoverable. A bad user request that
  was rejected. A soft error the kernel handled.
- **`ERR`** — something is wrong and a caller will see failure.
  Allocation failure, driver init failure, corrupted metadata.
- **`CRIT`** — the kernel cannot continue correctly. Usually paired
  with a panic.

### 2.3 Gating

Debug and trace logs are **compile-time gated**. Zero cost in
release.

```c
#ifdef CONFIG_DEBUG_SCHED
# define sched_dbg(fmt, ...) pr_debug("sched", fmt, ##__VA_ARGS__)
#else
# define sched_dbg(fmt, ...) do { } while (0)
#endif
```

Per-subsystem `CONFIG_DEBUG_*` flags in the build. The master
`CONFIG_DEBUG` flag pulls in all of them for the "debug everything"
builds.

**Never ship debug logging enabled in release.** The point of
compile-time gating is zero overhead on the hot path.

### 2.4 What to log

- **Subsystem init and shutdown.** `INFO`.
- **State transitions** in state machines. `DEBUG` normally, `INFO`
  if the transition is rare/important.
- **Errors the caller will not report upward.** `WARN` or `ERR`
  depending on severity.
- **Recoverable anomalies** (retry succeeded, fallback taken).
  `WARN`.
- **Every panic path.** `CRIT` with full context before the hang.

### 2.5 What not to log

- Anything in a tight hot-path loop at `INFO` or above. Hot-path
  logs are `DEBUG` only and compiled out in release.
- Anything that leaks secrets (credentials, keys, user data).
- "Entering function" / "exiting function" noise — use tracepoints
  (§4) for that, not logs.

### 2.6 Anti-patterns

- Logging without a subsystem tag — `pr_info("hello")` with no
  context.
- Unstructured free-form messages that grep can't carve up.
- Logs that say "error" but don't say *which* error or *where*.
- `printk`-equivalent calls on hot paths in release builds.
- Logs stripped of timestamps or CPU ids "to save space" — you lose
  the ability to correlate events.

---

## 3. Panic handling

### 3.1 Behavior

A kernel panic:

1. **Disables interrupts** on the panicking CPU.
2. **IPIs other CPUs** to halt them (so multi-core state is frozen).
3. **Dumps rich context** to serial (§3.2).
4. **Hangs in a `hlt` loop.** Does **not** reboot. Does **not** drop
   to a monitor.

The hang is deliberate: it leaves the system in a state GDB can
attach to and inspect. Reboot-on-panic throws away the evidence.

### 3.2 Panic output

Every panic dumps, in this order:

1. **The panic message** — one line, what tripped the panic.
2. **CPU state** — all GP registers, RIP, RSP, RFLAGS, CR0/CR2/CR3/
   CR4, current CS/DS/SS.
3. **Stack backtrace** — symbolized if possible (§5).
4. **Current task** — pid, name, state, kernel stack pointer.
5. **Lock state** — which locks this CPU holds, in acquisition order.
6. **Recent log ring** — the last N log lines (e.g., 64) from the
   in-memory ring buffer, in case serial was behind.
7. **Event trace tail** — last N events from the trace ring (§4.2).

Output is plain ASCII. No color codes on serial — they make logs
painful to grep and diff.

### 3.3 Attaching GDB after panic

```
(gdb) target remote :1234
(gdb) bt           # confirm the backtrace matches the serial dump
(gdb) info reg     # registers at panic
(gdb) thread apply all bt    # all CPUs if SMP
```

From here, use the tooling in §6.

### 3.4 Anti-patterns

- Rebooting on panic. You lose the state that caused the bug.
- Panic messages without the offending values (say *what* was
  invalid, not just *that* something was invalid).
- Dropping to a monitor that requires typing — you can't script it.
- Truncated backtraces because the stack walker gave up at the first
  unreadable frame. Walk as far as possible and print `???` for
  unreadable frames, don't stop.

---

## 4. Structured tracing

Two complementary mechanisms. Both optional per-subsystem at compile
time, both zero-cost when disabled.

### 4.1 Tracepoints

Named hook points in the kernel that emit structured events. Placed
at significant transitions:

- **Syscall entry/exit** — number, args, return value, task.
- **Scheduler** — pick, preempt, enqueue, dequeue.
- **IPC** — send, recv, reply.
- **VM** — page fault, map, unmap.
- **Filesystem** — read, write, open, close.

Tracepoints emit into the event ring (§4.2) and can optionally
invoke a user-provided handler for custom instrumentation.

```c
TRACE_POINT(sched_switch, pid_t prev, pid_t next)
```

### 4.2 Event ring

Fixed-size circular buffer of structured events. Lock-free per-CPU
ring, drained or dumped on demand.

- **Per-CPU** — avoids cross-CPU cache-line ping-pong on hot paths.
  See `PRINCIPLES.md §5.3` on false sharing.
- **Fixed-size entries** — no variable-length allocation on the hot
  path.
- **Dumped on panic** as part of the panic output.
- **Inspectable via GDB** — walk the ring in the post-panic state
  to reconstruct what the system was doing in the last few
  milliseconds.

### 4.3 When to use tracing vs logging

| Use case                          | Mechanism    |
|-----------------------------------|--------------|
| Human-readable operational event  | Log (§2)     |
| High-frequency hot-path event     | Tracepoint   |
| Panic post-mortem reconstruction  | Event ring   |
| Correlating cross-subsystem flow  | Tracepoints  |

If you find yourself logging thousands of lines per second, you
want a tracepoint, not a log.

### 4.4 Anti-patterns

- Tracepoints that allocate. The hot path must not touch the
  allocator.
- Event entries with variable-sized payloads. Fixed-size only.
- Global (non-per-CPU) event rings — you've just introduced
  contention into every traced hot path.

---

## 5. Stack walking

Two-tier strategy: fast path via frame pointers, fallback via DWARF.

### 5.1 Frame-pointer fast path

- **Compile kernel with `-fno-omit-frame-pointer`.** The one-register
  cost (RBP) is negligible and the debuggability payoff is huge.
- **Walker logic:** follow the `%rbp` chain, reading `[rbp]` for the
  previous frame and `[rbp+8]` for the return address. Stop at
  NULL, on an unreadable address, or after a sensible depth limit
  (e.g., 128).
- **Symbolize** each return address against the kernel symbol table.

This path is fast enough to run inside a panic handler.

### 5.2 DWARF fallback

- Used when the frame-pointer chain is broken — missing frame
  pointers in an asm trampoline, corrupted stack, etc.
- Parses DWARF CFI (`.eh_frame` / `.debug_frame`) to unwind without
  frame pointers.
- Slower and heavier. Not used in the panic fast path; used
  post-panic via GDB or offline.

### 5.3 Symbolization

- Kernel builds emit a symbol table consumable by the stack walker.
- Output format: `<function>+<offset> (<file>:<line>)` when line
  info is available; `<function>+<offset>` otherwise.
- Raw RIPs with no symbol — print as `0x<hex>` and let offline
  tooling resolve them.

### 5.4 Anti-patterns

- Compiling the kernel with `-fomit-frame-pointer` to "save a
  register." Not worth the debug cost.
- A stack walker that panics on an unreadable frame. Walkers must
  be robust against corrupted stacks — that's often *why* you're
  walking.
- Printing raw RIPs without attempting symbolization.

---

## 6. GDB and QEMU monitor

### 6.1 Launching

QEMU with:

```
qemu-system-x86_64 ... -s -S
# -s:   gdbserver on tcp:1234
# -S:   pause at start, wait for GDB
```

Then:

```
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) hbreak kmain
(gdb) continue
```

### 6.2 Useful GDB patterns

- **`hbreak`** (hardware breakpoint) in early boot before paging is
  fully set up. Software breakpoints can fail before the kernel
  image is resident.
- **`watch *(uint64_t *)0xADDR`** — hardware watchpoint on a
  specific memory word. Invaluable for catching "who wrote to this
  field?" bugs. Limited to 4 hardware watchpoints on x86.
- **`awatch`** — watch on read *and* write. Use when you don't know
  whether the corruption is a bad read or a bad write.
- **`x/16gx $rsp`** — dump the top of the stack as 64-bit hex.
- **`info registers`** — all regs, including segment and control.
- **`p/x *(struct task *)0xADDR`** — pretty-print a kernel struct at
  a given address.

### 6.3 QEMU monitor

Enter the monitor with `Ctrl-a c` in a text console, or use
`-monitor stdio` / `-monitor telnet::45454,server,nowait`.

Useful commands:

- **`info registers`** — CPU state without GDB.
- **`info mem`** — current paging structure summary.
- **`info tlb`** — TLB contents.
- **`x /<count><format> <addr>`** — dump physical memory. Note:
  monitor `x` reads *physical* memory; GDB `x` reads *virtual*.
  This distinction matters for diagnosing paging bugs.
- **`xp /<count><format> <addr>`** — explicit physical memory dump.
- **`gpa2hva <gpa>`** — translate guest physical to host virtual.
  Useful when pairing GDB with the monitor.
- **`stop` / `cont`** — pause/resume the guest without GDB.
- **`screendump <file>`** — grab a framebuffer snapshot for
  early-boot VGA debugging.

The monitor is especially useful for **paging bugs**: when a virtual
address is unreachable from GDB because the PT entry is wrong, the
monitor can still read it physically.

### 6.4 Anti-patterns

- Debugging paging bugs with GDB alone. The monitor is the right
  tool for "what's actually in this physical page?"
- Burning all 4 hardware watchpoints on incidental state. Save them
  for the actual suspect.
- Setting software breakpoints in non-resident code (very early
  boot, or in memory that'll be remapped) — use `hbreak`.

---

## 7. Assertions

### 7.1 Two forms

- **`ASSERT(cond)`** — invariant check. In debug builds, fires a
  panic with the condition, file, and line. In release builds, logs
  a `WARN` and continues.
- **`ASSERT_CRIT(cond)`** — safety-critical invariant. Panics in
  both debug and release. Use when continuing would corrupt state
  or violate a security boundary.

### 7.2 Rules

- **Assert internal invariants, not external input.** External
  input gets validated and returns an error (see `PRINCIPLES.md §4`).
  An assert firing means the code is wrong — not the caller.
- **Assert messages are informative.** Include the value or the
  context, not just the condition:

  ```c
  ASSERT(queue->head != NULL && "runqueue head invariant violated");
  ASSERT(task->state == TASK_RUNNING);
  ```

- **Assert at the first point the invariant is detectable.** Catch
  corruption early, not three function calls later.

### 7.3 Anti-patterns

- `ASSERT` on user-supplied values. That's a validation check, not
  an invariant.
- Asserting and then continuing as if the invariant held.
- Asserts with side effects — `ASSERT(do_stuff() == 0)` breaks in
  release when the assert compiles out.
- Using `ASSERT_CRIT` for non-critical conditions. If it panics in
  release, it had better be genuinely unsurvivable.

---

## 8. Memory debugging

### 8.1 Poisoning

- **Freed memory is filled with a poison pattern** in debug builds.
  Common choices: `0xDEADBEEFDEADBEEF` for 8-byte words,
  `0xCC` for byte fills (also `int3` on x86 — crashes fast if
  executed).
- **Freshly allocated memory** is poisoned with a different pattern
  (`0xAAAAAAAAAAAAAAAA`) so use-before-init is distinguishable from
  use-after-free in crash dumps.
- **Slab allocator** poisons the redzone around each object to
  catch buffer overflows.

### 8.2 Sanitizers

Where feasible, compile debug builds with:

- **UBSan** — undefined behavior (signed overflow, misaligned
  access, shift out of range, etc.). Essentially free and catches
  real bugs. See `PRINCIPLES.md §4.4`.
- **AddressSanitizer / KASAN-style shadow memory** — out-of-bounds
  and use-after-free. Expensive but invaluable for tracking down
  silent corruption.

Sanitizers are debug-build only. Release builds ship without them.

### 8.3 Hardware watchpoints

Four hardware watchpoints available on x86 via DR0–DR3. Use them
for:

- **"Who is writing this field?"** — set a write watch on a struct
  member and let the hardware catch the culprit.
- **Stack corruption** — watch the return address slot of a known
  frame.
- **Guard pages** — watch the first byte past a buffer.

Hardware watchpoints don't slow the system down between fires, so
they're usable in hot paths where software instrumentation would
distort the bug.

### 8.4 Anti-patterns

- Debug builds without poisoning — you miss free-then-use bugs that
  "happen to work" because the freed memory looked plausible.
- Running sanitizers in release. They're a debugging tool, not a
  deployment tool.
- Burning hardware watchpoints on low-value addresses and then not
  having one available when you actually need it.

---

## 9. Methodology: first things to check

When a bug lands on your desk:

1. **Read the serial log before touching anything else.** The panic
   message + last few lines usually narrow it to one subsystem.
2. **Is it reproducible?** Deterministic or flaky?
   - Deterministic → good. Minimize the repro.
   - Flaky → suspect a race, uninitialized memory, or
     timing-dependent code. Run under different loads.
3. **What changed?** `git log` on the affected subsystem since the
   last known-good run. Regressions usually have a short suspect
   list.
4. **Does it reproduce with debug build + sanitizers?** If yes,
   they'll likely catch it earlier and more precisely.
5. **Is it SMP-specific?** Run with `-smp 1`. If the bug vanishes,
   it's a concurrency bug — locking, memory ordering, or shared
   state.
6. **Is it optimization-specific?** Build at `-O0` and re-run. If
   the bug vanishes, suspect UB or a missing `volatile`.
7. **Where's the last known-good state?** Bisect between it and the
   current state.

### 9.1 Panic-specific checklist

- **Page fault?** Decode the error code. Present/not-present,
  read/write, user/supervisor, instruction fetch. Check CR2 for the
  faulting address.
- **GPF?** Look at the selector in the error code. Bad segment,
  bad descriptor, or an instruction that can't execute in the
  current ring.
- **Double fault?** The stack or TSS is trashed, or an exception
  fired while already handling one. Check the IST stack.
- **Triple fault?** The double-fault handler itself faulted. You
  are debugging without a safety net; GDB + QEMU monitor are your
  only tools.
- **Stack overflow?** Check RSP against the current task's stack
  bounds. Kernel stacks are small; a deep recursion or a large
  stack-allocated struct will blow them.

### 9.2 Concurrency-specific checklist

- **Does it go away with `-smp 1`?** → concurrency bug.
- **Does it go away with a big-kernel-lock wrapped around the
  suspect path?** → missing or wrong lock.
- **Does it go away in debug builds?** → timing-dependent; debug
  overhead serializes enough to hide it. The bug is still there.
- **Run with tracing on.** The event ring will often show the
  interleaving.

### 9.3 Anti-patterns

- Guessing at the fix before understanding the bug.
- "It's flaky, let me add a retry" — you've hidden the bug, not
  fixed it.
- Disabling an assert because it's inconvenient. The assert is
  telling you the code is wrong.
- Fixing the symptom in the crash site when the real bug is three
  layers up.

---

## 10. For AI agents specifically

1. **Read serial output before suggesting fixes.** If a log was
   provided, reference it specifically — quote the relevant lines
   in the response.
2. **Form a hypothesis before proposing changes.** State what you
   think is wrong and why, then propose the fix. Don't pattern-match
   to "looks like a null check is missing."
3. **Distinguish symptoms from root causes.** If the user reports a
   crash at point A but the data was corrupted at point B, say so.
   Fixing A hides the bug.
4. **Do not disable or weaken assertions** to "make it work." An
   assert firing is information.
5. **When debugging concurrency**, explicitly consider memory
   ordering, lock ordering, and interrupt context. Say which you've
   considered.
6. **Cite the debugging tool that would confirm the hypothesis.**
   "A watchpoint on `task->state` would confirm this" is better
   than "probably a race somewhere."
7. **Do not invent log lines or stack frames.** If you don't have
   the data, ask for it.

---

## 11. Summary

> Read serial first. Then GDB. Never only one.
> Panic dumps everything and hangs — don't reboot.
> Debug logs are compile-time gated. Zero cost in release.
> Frame pointer walks fast; DWARF walks when FP breaks.
> Assert internal invariants. Validate external input.
> Poison freed memory. Run sanitizers in debug.
> Form a hypothesis. Confirm with tools. Then fix the root cause.
