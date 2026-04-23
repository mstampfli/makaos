# Engineering Principles

Rules for how code is **built**, not how it looks. This is the
architecture and decision-making document. When `CODE_STYLE.md` tells
you where to put a brace, this file tells you whether to write the
function at all.

These are **strong defaults**. A principle can be violated when the
violation is documented, justified, and reviewed. Silent violations
are bugs.

---

## 1. The priority stack

When principles conflict, resolve in this order:

1. **Performance.** This is a systems project. Performance is the
   reason it's written in C and NASM instead of something safer.
   Every design decision starts with "what's the fastest correct way
   to do this?"
2. **Correctness.** A fast wrong answer is still wrong. Performance
   wins every tie *except* correctness — a faster path that corrupts
   state is not faster, it's broken.
3. **Scalability / clean abstractions.** Code is written once but
   read, extended, and debugged many times. Modular, composable
   subsystems beat monolithic ones — *provided the abstraction
   doesn't cost performance on the hot path.*
4. **Readability.** Readable code is a goal, not a veto. When
   performance and readability genuinely conflict, performance wins.
   Document the clever code; don't dumb it down.

Read §5 carefully — "design for performance" means upfront, not
"measure first, then optimize."

---

## 2. No hacks. Ever.

There is no "temporary" hack. There is no "we'll fix it later." There
is no "just for now." A hack shipped is a hack that stays.

**Always solve the problem properly on the first attempt.** If you do
not know the proper solution, the correct move is:

1. Stop writing code.
2. Understand the problem.
3. Design the proper solution.
4. Then write it.

If the proper solution is out of scope for the current change, the
current change is the wrong change. Split it, deprioritize the
blocker, or expand the scope — but do not ship a hack.

### What counts as a hack

- A workaround for a bug elsewhere that you didn't file and fix.
- A special case inside a general function ("if `name == "root"` …").
- A hardcoded constant that should be derived.
- Code that "works for now" because the inputs happen to be small.
- Any line of code you'd be embarrassed to explain in a code review.

### The one real exception

If a hack is the **only** way to get past a blocker for something
strictly more important (e.g., the system won't boot at all), it is
acceptable **if and only if**:

1. It is marked with a `HACK:` comment explaining exactly what's wrong
   and what the real fix looks like.
2. A tracking issue exists and is linked.
3. The hack is as narrowly scoped as possible.

This exception is rarely invoked. If you find yourself invoking it
twice in a week, you are invoking it wrong.

---

## 3. Modularity

### 3.1 Dependency rules

- **Modules form a DAG.** No cycles. Ever.
- If subsystem A depends on subsystem B, B must not — directly or
  transitively — depend on A.
- **Detecting a cycle is a hard stop.** Refactor, introduce an
  interface, or invert the dependency. Do not paper over it with a
  forward declaration.

### 3.2 Interfaces

- **Each subsystem exposes a minimal public API** through a single
  public header (or a small set).
- Everything else is private — `static` functions, headers under the
  subsystem directory, internal structs.
- **Public structs are acceptable** when they are stable and the
  consumer genuinely needs the fields. Opaque handles are preferred
  when the layout might change or when the consumer has no business
  touching internals.
- **Do not expose internal types just because it's convenient.** The
  cost is paid every time the internals change.

### 3.3 Abstraction

- **A little over-engineering is better than a lot of rework.** If you
  can see a second use case coming within the next few weeks, design
  for it now.
- **But**: "I might someday need X" is not a second use case. A real
  second consumer or a concrete planned feature is.
- **Prefer composition over inheritance-of-state.** Pass dependencies
  in, don't reach for globals.

### 3.4 When in doubt, smaller

A small, well-named module with a clear responsibility is almost
always better than a large one that does "everything related to X."

---

## 4. Correctness

### 4.1 Validation

- **Validate at trust boundaries, not everywhere.** A syscall entry
  point validates user input. The internal function it calls trusts
  that validation.
- **Boundaries include**:
  - Syscall entry (user → kernel).
  - Hardware interfaces (device → driver).
  - IPC receive points (foreign task → local).
  - Deserialization (bytes → struct).
- **Inside the trust boundary**, use asserts and invariants to catch
  bugs during development, not defensive runtime checks.

### 4.2 Asserts and invariants

- **Assert internal invariants** that should always hold if the code is
  correct. If an assert fires, the code is wrong — not the input.
- **Never assert on external input.** Validate it and return an error.
- **Asserts should be informative.** Prefer `ASSERT(ptr != NULL &&
  "ready queue head must be non-NULL")` over bare `assert(ptr)`.

### 4.3 Error propagation

- **Errors propagate up** as negative errno (see `CODE_STYLE.md` §1.11).
- **Never swallow an error.** If you can't handle it, return it. If
  there's genuinely nothing the caller can do and the error is
  non-fatal, log it — but swallowing without logging is a bug.
- **Never `goto` past cleanup.** Every resource acquired on the success
  path must have a matching release on every failure path reachable
  after acquisition.

### 4.4 Undefined behavior

- Treat UB as a bug regardless of whether it currently "works."
- Integer overflow on signed types, strict-aliasing violations,
  out-of-bounds reads — these aren't style issues. They're correctness
  bugs that are invisible until they aren't.

---

## 5. Performance

### 5.1 The rule

**Always optimize. Performance is the #1 goal.**

Every design decision starts with "what's the fastest correct way to
do this?" — not "make it work first, optimize later." By the time
"later" arrives, the data structures are wrong, the allocation
strategy is wrong, and the hot path is full of cache misses that
weren't considered upfront.

- **Data structures, memory layout, allocation strategy, and algorithm
  choice are chosen for performance from the start.**
- **Measure to confirm the optimization worked**, not to decide
  whether to optimize. "I think this is fast" is not a statement
  about performance. It's a statement about your intuition.
- **Every hot path is benchmarked** under representative workloads.

"Premature optimization is the root of all evil" is a misquoted
aphorism that does not apply to this codebase. Here, the lack of
upfront performance thinking *is* the evil.

### 5.2 Complexity is a tool, not a god

Big-O is a starting point, not an answer. It ignores constants, cache
behavior, branch prediction, TLB pressure, and real input sizes.
**Choose the algorithm that wins on the actual workload**, not the
one with the prettiest asymptotic bound.

- For small `n`, a linear scan over a contiguous array is often
  **faster** than an `O(1)` hash lookup — the array fits in L1 and
  the hash doesn't.
- An `O(log n)` tree can be slower than an `O(n)` array for `n < 32`
  because of pointer chasing and cache misses.
- A theoretically optimal algorithm with high constant factors can
  lose to a "worse" algorithm on realistic inputs.

**The decision procedure:**

1. What's the expected input size and distribution?
2. What are the candidate approaches?
3. Which has the best *measured* performance on this workload?
4. Pick that one. Document why.

If a hot-path function is labeled `O(n)` because `n` is bounded and
small and the linear scan wins on cache, say so in the doc comment.
Someone will come along later and "fix" it to `O(1)` with a hash
table otherwise, and make it slower.

### 5.3 Cache-awareness is not optional

Memory is the bottleneck. The CPU waits for memory orders of magnitude
longer than it waits for compute. Data layout is a first-class design
concern, not a polish pass.

- **Lay out hot data for cache locality.** Fields accessed together
  live together. A 64-byte cache line is the unit of thought for
  struct design.
- **Prefer SoA (struct-of-arrays) over AoS (array-of-structs)** when
  iteration touches only a few fields. Iterating an array of
  100-byte structs to read one `u32` field wastes 96 bytes of cache
  line per element.
- **Align hot structures** to cache line boundaries. Hot fields that
  are written on one CPU and read on another go on separate cache
  lines to avoid false sharing.
- **Hot/cold split.** If a struct has fields touched every call and
  fields touched rarely, split them. Put the cold fields behind a
  pointer or in a separate array. The hot struct gets smaller, more
  of it fits in L1, the hot path gets faster.
- **Pad to avoid false sharing** on per-CPU or per-thread data:
  `__attribute__((aligned(64)))` on structures that will be written
  concurrently from different cores.
- **Prefetch** when the access pattern is predictable and the compiler
  can't figure it out. Don't guess — measure whether it helps.

### 5.4 Hot paths

- **Identify hot paths explicitly.** The scheduler dispatch loop, the
  page fault handler, the syscall entry path, the IPC send path — these
  get more design attention, more review, more benchmarking.
- **Hot paths are benchmarked.** Not every function needs a benchmark;
  a hot path without one is a hot path you're not actually measuring.
- **No heap allocation on hot paths.** Stack, arena, or pool
  allocation is faster and more predictable. `kmalloc` in the
  scheduler is a bug.
- **Minimize branches on hot paths.** Likely/unlikely hints where the
  branch is heavily skewed. Branchless code where the branch is
  unpredictable.
- **Inline aggressively on hot paths.** `static inline` for small
  helpers called from hot code. Cross-translation-unit inlining where
  the build supports it.

### 5.5 Memory

- **Static allocation** for anything whose maximum size is known at
  compile time. Zero allocator overhead, predictable layout, no
  fragmentation.
- **Dynamic allocation** for anything exposed to user input or whose
  size is genuinely unbounded. This is the common case for
  user-facing subsystems.
- **Bound every dynamic allocation.** An unbounded `kmalloc` based on
  user-supplied size is a DoS vector and a correctness bug.
- **Use arena/slab/pool allocators** for collections of small,
  same-sized objects. Malloc/free per object has overhead,
  fragmentation, and cache cost. A slab allocator amortizes all of it.

### 5.6 Anti-patterns

- "Make it work first, optimize later." Later never comes with the
  right data structure.
- Optimizing without measuring — you don't know what's actually slow.
- Ignoring cache behavior because "the compiler will handle it." The
  compiler cannot restructure your data layout.
- Array-of-structs with a 200-byte struct that you iterate to read one
  field. That's a cache disaster.
- Heap allocation on the hot path because it was convenient.
- `O(1)` that's slower than `O(n)` for the actual `n`, adopted because
  "Big-O says so."
- Benchmarks that aren't representative of real workloads.
- Caching without an eviction policy.

---

## 6. Concurrency

### 6.1 Prefer lock-free

- **Use locks only when they're genuinely the fastest option.** For
  read-mostly data, RCU is usually better. For hot counters, atomics.
  For multi-producer/single-consumer queues, a lock-free ring.
- **Locking is not a synonym for "thread-safe."** A lock is a specific
  implementation of thread safety, and often the slowest one.

### 6.2 When you do use locks

- **Fine-grained** is the default. A per-object lock beats a
  subsystem-wide lock for contention, at the cost of more complex
  acquisition ordering.
- **Document lock ordering** wherever multiple locks can be held
  simultaneously. Lock-ordering violations cause deadlocks.
- **Hold locks for the minimum duration.** Compute outside the
  critical section when possible.
- **Never call into another subsystem with a lock held** unless that
  subsystem's API documents it as safe.

### 6.3 Context

- **Critical-path functions document their context**: IRQ-safe or not,
  may-sleep or not, which locks the caller must hold, which locks are
  taken internally.
- **Non-critical functions** can rely on context being obvious from
  the code, but if it's not obvious within 30 seconds of reading,
  document it.

### 6.4 Anti-patterns

- Taking a sleeping lock in an IRQ context.
- Calling into user space with a lock held.
- Locks whose purpose isn't documented.
- "I'll figure out the locking later" — no, you'll figure it out now.

---

## 7. Testing

### 7.1 The approach

- **In-kernel self-tests**: boot-time sanity checks for invariants,
  data structure correctness, and core algorithms. These run every
  boot in debug builds.
- **Integration tests at subsystem boundaries**: every public API of
  every subsystem has at least one integration test covering its
  success path and its most important failure modes.
- **Hot-path tests**: any hot path has a test that exercises it under
  representative load.

### 7.2 What isn't tested

- Trivial static helpers.
- Pure formatting/logging.

Everything else should have a test. If it's hard to test, it's
probably badly designed — the test difficulty is a design signal.

### 7.3 Tests are code

The code-style rules apply to tests. A sloppy test is a future bug
source disguised as a safety net.

---

## 8. Design decisions

### 8.1 When the rules run out

If this document and `CODE_STYLE.md` don't answer your question:

1. Ask: which option maximizes the priorities in §1, in order?
2. If unclear, ask: which option is the least surprising to a reader
   who understands the codebase?
3. If still unclear, **stop and ask a human** (or flag it clearly in
   the commit message so a human will).

### 8.2 Patterns

- **Prefer existing patterns** already used in the codebase. An
  inconsistent codebase is harder to navigate than a mediocre but
  consistent one.
- **Only invent a new pattern** when no existing one fits. When you
  do, document the pattern so the next person can reuse it instead of
  inventing a third.

### 8.3 Dependencies

- **Every new external dependency must be justified.** Added
  maintenance burden, added attack surface, added build time.
- **Prefer writing small focused code** over pulling in a big library
  for a small feature.
- **Stdlib and project internals** are not "dependencies" in this
  sense — using them is default behavior.

### 8.4 Reading before writing

- **Read the surrounding code** before modifying anything. Understand
  the conventions, the invariants, and the neighbors before touching
  the function.
- **Read the header** of any function you call. If it has a context
  requirement (IRQ-safe, lock held, etc.), obey it.
- **Read the git log** for the file if the change is non-trivial.
  History tells you why things are the way they are.

---

## 9. For AI agents specifically

These rules apply to any agent (Claude Code, etc.) generating or
modifying code in this codebase.

1. **Do not touch `do-not-touch` areas** without explicit permission in
   the current session. These are flagged in comments at the top of
   the file or function, or in `MAKAOS.md`.
2. **Justify any new dependency or abstraction.** Before adding a new
   header, a new subsystem, or a new external library, state — in the
   response — why an existing one doesn't work.
3. **Read the surrounding code before changing anything.** Not one
   function above and below. The whole file, and the headers it
   depends on.
4. **Invent a new pattern only when no existing one fits.** If you
   find yourself writing something that looks unlike the rest of the
   codebase, stop and check whether you're missing a convention.
5. **Ask when uncertain, don't guess.** A question costs a round trip.
   A wrong guess merged into the codebase costs a debugging session.
6. **Never silently deviate from this document.** If you think a rule
   should be broken, say so explicitly in the response and explain
   why.

---

## 10. Summary

> Performance first — design for it upfront, measure to confirm.
> Correctness is non-negotiable. Keep modules small and layered,
> validate at the edges, trust the middle. Cache-aware data layout
> is not optional. Prefer lock-free where it wins, lock fine-grained
> where it must. No hacks. When in doubt, read first and ask second.
