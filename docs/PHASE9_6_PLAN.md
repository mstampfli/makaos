# Phase 9-6 — Cross-CPU wait-queue audit + re-enable round-robin placement

## Goal, in one sentence

Make every sleep/wake path in MakaOS SMP-correct with **zero new
global locks**, then flip `pick_home_cpu()` back to round-robin so
user tasks actually run on APs.

## Non-goals

- TLB shootdown — that's Phase 9-7.
- Load balancing / work stealing — that's Phase 9-8.
- Per-CPU slab magazines — that's Phase 4's separate redesign.
- New syscalls or userland changes.
- Any new data structure.  The existing `wait_queue_t` is sufficient;
  we are only auditing *use sites*.

## The single rule

> A waker commits its side effect **before** it publishes the wake.
> A sleeper publishes its presence on the wait queue **before** it
> re-checks the condition.  Both halves protected by the same
> `spin_lock_irqsave(&home->rq_lock)` on the sleeper side (via
> `sched_sleep`) and `atomic_store_release`/lock-free queue semantics
> on the waker side (via `wait_queue_wake_all`).

Every subsystem we touch must conform to exactly this shape.  When
it does, Phase 9-5's scheduler-level `wake_pending` protection +
the `wait_queue_t` lock-free MPSC push catches every racy
interleaving with zero extra cost.

## Why this works with no new locks

The scheduler primitive already gives us everything we need:

1. `sched_wake(t)` takes `home->rq_lock` once, observes `t->state`
   atomically under that lock, and either moves `t` from sleep-head
   to rq (state==SLEEPING) or sets `t->wake_pending=1` (any other
   state).  Phase 9-5 commit `7b985f1`.

2. `sched_sleep()` takes the SAME lock, checks `wake_pending` while
   holding it, and if set bails out without committing to sleep.
   Same commit.

3. `wait_queue_t` (kernel/wait.c) is lock-free on the hot path:
   `wq_add` is a single CAS-push onto the head, `wait_queue_wake_all`
   is a single `xchg` that detaches the whole chain, then walks it
   privately calling `task_wake_func` → `sched_wake` on each entry.

All three primitives are already SMP-safe.  The problem is that
**we use them wrong in several places**: the wakers set the condition
AFTER firing `wake_all`, or the sleepers re-check the condition
BEFORE registering on the queue, or both.  Phase 9-6 is the audit
that makes every use site obey the rule above.

---

## Phase 9-6 does NOT add any of the following

- ❌ No global "wait table" hash.
- ❌ No new per-fd spinlock.
- ❌ No hazard pointers.
- ❌ No atomic seqcounts on rx rings.
- ❌ No RCU on wait queues.
- ❌ No IPI-based rendezvous primitive.

If a subsystem cannot be made correct with the existing primitives
applied correctly, we revisit the design — but that case should not
arise.  Every race we've seen so far comes from wrong *ordering*, not
missing locks.

---

## The canonical correct pattern (memorise this)

```c
// ── SLEEPER ────────────────────────────────────────────────────────
for (;;) {
    // Phase 1 — quick check.  No registration cost; if the data is
    // already there we don't touch the wait queue at all.
    if (cond_ready()) break;

    // Phase 2 — register BEFORE the real check.
    task_we_t we;
    task_we_init(&we, g_current);
    task_we_add(&wq, &we);

    // Phase 3 — re-check UNDER the registration.  If the waker
    // committed its side effect between phase 1 and our task_we_add,
    // one of three things is true:
    //   (a) we observe cond_ready() here and don't sleep,
    //   (b) the waker already ran wait_queue_wake_all and our entry
    //       was in the chain — task_wake_func → sched_wake set our
    //       wake_pending, so sched_sleep bails out,
    //   (c) the waker ran wake_all and we weren't yet in the chain
    //       but the waker's cond_ready write happened before our
    //       task_we_add's CAS release, so phase 3's re-check sees it.
    // Every interleaving maps to (a), (b), or (c).  No lost wakeup.
    if (cond_ready()) {
        task_we_remove(&wq, &we);
        break;
    }

    sched_sleep();

    task_we_remove(&wq, &we);

    if (signal_has_actionable(&g_current->sigstate))
        return -EINTR;
}

// ── WAKER ──────────────────────────────────────────────────────────
// Commit the side effect FIRST (under whatever mutex guards the
// shared state — ring buffer head, flag, etc.), THEN fire wake_all.
commit_cond_ready();          // e.g. push byte, bump ring tail, set flag
wait_queue_wake_all(&wq);     // lock-free drain + sched_wake each
```

**Every deviation from this pattern is a bug.**  Phase 9-6 tracks down
every deviation, fixes it, and documents the fix inline.

### Why each phase is load-bearing

- **Phase 1 fast path** — performance, not correctness.  Skip it if
  measurement shows no win; correctness stays intact.
- **Phase 2 (register before re-check)** — guarantees the waker's
  `wake_all` sees our entry.  Without it: classic lost wakeup.
- **Phase 3 (re-check under registration)** — catches the case where
  the waker set `cond_ready=1` before we added ourselves to the queue
  (so its `wake_all` found no waiters).  Phase 1's scheduler-level
  `wake_pending` handles the mirror case where the waker set
  `cond_ready=1` AFTER we added but BEFORE we reached `sched_sleep`.
- **`task_we_remove` on every exit path** — stack-allocated entries
  must leave the queue before their frame is dropped.
- **Waker commits BEFORE wake_all** — every sleeper that's already
  in the queue sees `cond_ready=1` on its next run.

### Barriers — who issues what

x86-64 TSO gives us store→store and load→load ordering for free.
The only reorder the hardware actually performs is store→load across
different addresses.  Every one of these is covered by our existing
primitives:

- `wq_add`'s CAS is `__ATOMIC_RELEASE` → every waker after it sees
  our `we` in the chain with all its fields initialised.
- `wait_queue_wake_all`'s `xchg` is `__ATOMIC_ACQ_REL` → the drainer
  sees every pre-xchg writer store (ring push, cond flag) before
  walking the chain.
- `spin_lock_irqsave(&rq_lock)` in `sched_sleep` is full acquire;
  the symmetric unlock is release.  Together they linearise any
  sleeper vs. waker race on the target's wake_pending flag.

**Phase 9-6 must not add new `smp_mb()` / `smp_wmb()` calls.**  If
one seems necessary, the use site is wrong — fix the ordering of
operations instead.

---

## Subsystems to audit (in order)

### 9-6a — waitpid child reap  (`sys_wait` in syscall.c)

**Why first:** already has a GDB reproducer from the Phase 9-5
session (`bash → sys_wait → sched_sleep` with a zombie child on a
remote CPU).

**Race:**
- Parent walks `children` list, sees child state=RUNNING, proceeds
  toward sched_sleep.
- Child on CPU X runs `sys_exit`: sets `state=TASK_ZOMBIE`,
  `sched_add_zombie`, `signal_send(parent, SIGCHLD)`.
- `signal_send` always calls `sched_wake` (Phase 9-5).  Under
  the current code this reaches the lock-held check in sched_wake
  and either moves parent from sleep_head to rq (if parent already
  reached sched_sleep) or sets `wake_pending=1` (if not).
- **Remaining exposure:** parent's `children` list walk is
  unsynchronised.  On UP it's fine (parent owns the list), but
  on SMP the child mutates the list when it reparents-to-init
  during its OWN exit.  That write is ordered before the
  signal_send/wake, but parent may be mid-walk when it happens.

**Fix:**
1. Keep the Phase 9-5 wake_pending protection (already in).
2. Make the `children` list modifiable only under the parent's
   per-task lock, OR audit every mutator (fork, exit, reparent) to
   confirm it only runs when the parent is either (a) the mutator,
   (b) quiesced in sys_wait just before re-walking.  Option (b)
   is free; option (a) requires one lock.
3. **Preferred:** option (b).  The only concurrent mutator is the
   child's own `sys_exit` reparenting, and it happens BEFORE the
   SIGCHLD / sched_wake.  Memory barrier in signal_send's lock
   release ensures parent's next walk sees the new links.
   Confirm with reproducer.

**Deliverable:** `sys_wait` uses the canonical pattern; no new lock.

**Reproducer:** `while :; do ls; done` inside bash on `-smp 4`
round-robin.  Should run thousands of iterations without hanging.

---

### 9-6b — pty slave_read / master_read  (`kernel/drivers/tty/pty.c`)

**Why:** user reports the hang is PTY-specific (pty shell hangs,
tty shell does not).  Intermittent missing prompts.

**Races identified in Phase 9-5 session:**
- `pty_slave_read` sleeps on `tty->waitq`.  Wakes from `tty_input_char
  → ldisc_flush_line → wait_queue_wake_all(&tty->waitq)`.  In raw
  mode every char wakes; in canonical mode only newline wakes.  Both
  commit the ring push BEFORE wake_all, so waker-side is correct.
- `pty_master_read` sleeps on `pty->master_waitq`.  Waker is
  `pty_slave_write` via `tty_output_buf` → `mb_push` + `wait_queue_wake_all`.
  Same shape, check commit order.
- `slave_drain_waitq` for full-ring backpressure.  Waker is
  `pty_master_read` after draining.

**Fix:**
1. Walk through each pair (reader/writer, sleeper/waker), verify
   the canonical pattern is used.
2. Fix any commit-after-wake ordering.
3. Confirm every stack-allocated `task_we_t` is unconditionally
   `task_we_removed` on every exit path (incl. signal EINTR).

**Deliverable:** pty passes an echo-and-run loop on SMP:
```
while :; do echo ok; done
```
inside bash running under makaterm, for 60 seconds without a hang.

---

### 9-6c — epoll_wait  (`sys_epoll_wait` in syscall.c)

**Why:** the `state->has_ready` flag has the nastiest ordering
window.  Bash's interactive shell inside makaterm uses epoll on
stdin and blocks there.

**Race:**
```
sleeper                          waker (writer side on any fd)
-------                          ----------------------------
state->has_ready = 0;            commit ring push
task_we_add(&state->wq, ...);    wait_queue_wake_all(&fd->waitq)
if (!state->has_ready) {           → persistent epoll_we_t fires:
  poll re-scan;                      state->has_ready = 1;
  if still empty:                    wait_queue_wake_all(&state->wq)
    sched_sleep();
}
```

If the waker's ring push happens AFTER the sleeper's `has_ready = 0`
but BEFORE the sleeper's `task_we_add`, the waker's wake_all finds
no waiters (fd waitq entry is persistent but fires
`epoll_wake_func` which re-sets `has_ready=1` and wakes
`state->wq` — but `state->wq` has no entry yet).  Then sleeper
re-reads `has_ready` — and it's 1!  So phase 3 of the pattern
catches it.

But **another** order: waker runs completely BEFORE the sleeper
clears `has_ready = 0`.  Then sleeper's clear overwrites the 1.
Re-check sees 0.  sleeper does the poll re-scan — if the ring
hasn't been drained by someone else, poll returns ready.  OK.
If the ring has been drained (two sleepers?), poll returns empty,
sleeper sleeps.

**Decision:** the `has_ready` flag is a cache, not a source of
truth.  The **source of truth** is each fd's `poll()` callback.
The flag exists only to let sleepers avoid a full re-scan when no
event fired since the last check.

**Fix:**
1. Re-order: the clear of `has_ready` moves INSIDE `task_we_add`'s
   atomic release window.  Use `__atomic_store_n(&has_ready, 0,
   __ATOMIC_RELAXED)` BEFORE `task_we_add`, and rely on the
   CAS-release to linearise with wakers.
2. The waker's write to `has_ready = 1` in `epoll_wake_func` becomes
   `__atomic_store_n(&has_ready, 1, __ATOMIC_RELEASE)`.
3. Phase-3 re-check reads with ACQUIRE.
4. The poll re-scan under `state->lock` is kept as the ultimate
   tiebreaker — spurious wakeups are cheap.

No new lock; the `state->lock` is an existing per-epoll-instance
lock, not global.

**Deliverable:** `while :; do :; done` in an interactive bash run
inside makaterm must not stall; epoll stdin reads must wake
promptly on every keystroke.

---

### 9-6d — pipe  (`kernel/fs/pipe.c`)

**Race:** both reader and writer sleep and wake each other.  Two
wait queues per pipe: `rd_waitq`, `wr_waitq`.  Pipe buffer is a
ring under the pipe's own spinlock (already correct for the ring
itself).

**Fix:**
1. Reader's sleep: canonical 3-phase pattern on `rd_waitq`.
2. Writer's sleep (on full ring): canonical 3-phase pattern on
   `wr_waitq`.
3. On read: drain ring byte(s), then `wait_queue_wake_all(&wr_waitq)`.
4. On write: push byte(s), then `wait_queue_wake_all(&rd_waitq)`.
5. Close path: flip `closed` flag, wake both queues.

**Deliverable:** `yes | head -1000000 >/dev/null` inside bash, with
`pick_home_cpu` round-robin.  Must not hang and must complete
promptly.

---

### 9-6e — unix socket (`kernel/net/unix_sock.c`)

**Same pattern as pipe.**  Two parties, each with its own ring and
its own wait queue.  Close half → wake both halves.

**Fix:** audit every `sched_sleep` call in `unix_sock.c` (there are
five — see Phase 9-5 session grep output) to use the canonical
pattern.  Commit ring writes before `wake_all`.

**Deliverable:** the display server / compositor uses unix sockets
(`userland/apps/display`).  Running makadisplay + a demo_client on
round-robin SMP must work end-to-end without hangs.

---

### 9-6f — AHCI I/O thread rendezvous  (`kernel/drivers/storage/ahci.c`)

**Race:**
- `ahci_submit` (caller context) pushes a request into `s_req_ring`
  and calls `sched_wake(s_io_thread)`.
- `ahci_io_thread` (kthread) sleeps until `s_req_head != s_req_tail`,
  processes the request, sets `r->done = 1`, `sched_wake(r->waiter)`.
- `ahci_submit` then polls `while (!r->done) sched_sleep()`.

**Neither side is a wait queue.**  Both rely on `sched_wake` directly.
Phase 9-5's wake_pending catches the sleeper/waker race, but we
should migrate to a proper wait queue for consistency and to make
multi-waiter safe (Phase 9-8 could run multiple submitters).

**Fix:**
1. Add `wait_queue_t io_thread_wq` for the kthread (so
   `ahci_submit` fires `wait_queue_wake_one` instead of direct
   `sched_wake`).
2. Each submitted request carries a stack `task_we_t` on its
   waiter.  `ahci_submit` uses the canonical pattern to wait for
   `r->done`.
3. `ahci_io_thread` uses the canonical pattern to wait for
   `s_req_head != s_req_tail`.

**Deliverable:** parallel disk reads from multiple CPUs complete
correctly:
```
for i in 1 2 3 4; do cat /bin/bash >/dev/null & done; wait
```

---

### 9-6g — re-enable round-robin placement

Once 9-6a through 9-6f all pass, **and only then**, flip
`pick_home_cpu()` back to round-robin in one tiny commit and rerun
every reproducer from 9-6a–f in sequence.

Commit is 3 lines of code.  The point is to keep the "enable SMP
scheduling" change **separate** from the subsystem fixes so we can
bisect if any regression appears.

---

## Testing infrastructure

Every subsystem audit commit must include its reproducer.  Keep
them as a shell script under `userland/apps/smp_test/`:

```sh
# userland/apps/smp_test/smp_test.sh
echo "[smp_test] waitpid loop"
for i in $(seq 1 1000); do ls >/dev/null; done

echo "[smp_test] pipe loop"
yes 2>/dev/null | head -100000 >/dev/null

echo "[smp_test] epoll loop"
# bash's read -t implements a short epoll_wait — hammer it
for i in $(seq 1 100); do read -t 0.01 || true; done

echo "[smp_test] parallel cat"
for i in 1 2 3 4; do cat /etc/passwd >/dev/null & done
wait

echo "[smp_test] OK"
```

Run this after every 9-6 commit on `-smp 4` round-robin.  Green →
commit lands.  Red → fix before committing.

GDB is the debugger of record; do NOT rely on printf-debugging
which distorts timing.  Every hang starts with

```sh
$ qemu ... -gdb tcp::1234 -S &   # start paused
$ gdb -ex 'target remote :1234' build/kernel.elf
(gdb) b sys_wait
(gdb) c
...reproduce...
(gdb) info threads
(gdb) bt
(gdb) p g_cpus[N].rq.sleep_head
...
```

---

## Order of operations

1. Write the `smp_test.sh` reproducer (commit on its own).
2. 9-6a: fix `sys_wait` children walk, pass waitpid reproducer.
3. 9-6b: fix pty read/write, pass pty reproducer.
4. 9-6c: fix epoll, pass interactive shell reproducer.
5. 9-6d: fix pipe, pass pipe reproducer.
6. 9-6e: fix unix socket, pass display-server reproducer.
7. 9-6f: fix AHCI rendezvous, pass parallel-cat reproducer.
8. 9-6g: flip `pick_home_cpu()` to round-robin, re-run all of
   the above in one go.  If green, update SMP_KNOWN_RACES.md
   entry 24 from "DEFERRED" to "RESOLVED", update PHASE9_HANDOFF.md,
   and land Phase 9-6.
9. Go on to Phase 9-7 (TLB shootdown).

---

## Hard constraints the user imposed

- **No locks unless absolutely necessary.**  Every audit item has
  to justify why it doesn't add a new lock (or explain why it must).
  Current count: we expect to add **zero** new locks in Phase 9-6.
- **No "simpler" / "for now" hacks.**  Each fix must be the final
  correct form.  No TODOs, no placeholder comments.
- **Hot paths must stay lock-free or per-CPU.**  The wait_queue_t
  primitive is already lock-free on push and drain.  Our fixes
  only change the *order* of operations, not the data structures.
- **Each phase = one commit.**  9-6a, 9-6b, ..., 9-6g each land as
  a single commit with its reproducer passing.

---

## Performance budget

Phase 9-5 added these per-operation costs that 9-6 must not make
worse:

- `sched_wake`: one `spin_lock_irqsave` + one atomic flag write +
  optionally one LAPIC ICR wrmsr (~500 cycles total if cross-CPU,
  ~60 cycles same-CPU).
- `sched_sleep`: one `spin_lock_irqsave` + wake_pending check +
  linked-list push + `do_switch`.
- `fb_term_putc`: one `spin_lock_irqsave` per byte, one per
  `write(1, ...)` via the batched `fb_term_write`.
- `wait_queue_wake_all`: one `xchg` + chain walk.  Unchanged from
  pre-9-5.

Phase 9-6 is pure re-ordering of use-site code + adding the
`register → re-check → sleep` triangle where it's missing.  **There
are no new heap allocations, no new MSR writes, no new atomics on
the hot path.**  If a fix adds any of those, the design is wrong —
revisit.

---

## Exit criteria

Phase 9-6 is DONE when all five are true:

1. `pick_home_cpu()` returns round-robin across `[0, g_num_cpus)`.
2. `smp_test.sh` passes 100 runs back-to-back on `-smp 4` without
   a hang, a PF-KILL, or a kernel panic.
3. `docs/SMP_KNOWN_RACES.md` entry 24 is marked RESOLVED.
4. `docs/LOCKS.md` reflects any new lock (expected: zero).
5. `docs/PHASE9_HANDOFF.md` is updated with the 9-6 state for the
   next `/clear`.
