# MakaOS-Specific Rules

Conventions that apply only to MakaOS. When these conflict with the
general guidelines in `CODE_STYLE.md` or `PRINCIPLES.md`, this file
wins — but any deviation from the general rules should be documented
here and nowhere else.

---

## 1. Build system

- **Use `bash build.sh`.** Not `make`. Not a fresh invocation of `gcc`
  from the command line. Not anything else.
- If the build fails, the fix is in the source tree or in `build.sh`
  itself — not in a workaround invocation.
- **Do not introduce a parallel build path** (a second `Makefile`, a
  CMake file, etc.) unless the existing `build.sh` is proven
  insufficient and the replacement is reviewed as a project-level
  change.

---

## 2. Do-not-touch areas

Some areas of the codebase have been stabilized after painful
debugging. They are flagged in-file with a comment starting
`/* DO NOT TOUCH */` and an explanation of why.

**Do not modify these regions without explicit permission in the
current session.** This includes:

- **Nested helper functions in `ext2.c`** — a stack overflow bug was
  traced to this pattern. The refactor that fixed it is load-bearing.
- **The `spawn_attr_t` inline unveil array** — sizing here is tied to
  a resolved stack overflow. Changing the size or structure reopens
  that class of bug.

If a `DO NOT TOUCH` region seems to need modification, **stop and ask**.
The correct response in 99% of cases is "don't." In the 1% case where
it really does need to change, the change should be paired with a
revision of the flag comment explaining the new invariant.

---

## 3. Subsystem boundaries

Current major subsystems and their public headers:

- **`kernel/sched/`** — scheduler, task management, PID table.
- **`kernel/mm/`** — memory management, heap, page allocator.
- **`kernel/fs/`** — filesystem layer, including `ext2.c`.
- **`kernel/svc/`** — `svcmgr` and service-lifecycle logic.
- **`kernel/sec/`** — `pledge`/`unveil` enforcement and attribute
  inheritance.
- **`kernel/ipc/`** — pipes, signals (including SIGPIPE delivery).
- **`kernel/drivers/`** — hardware drivers.
- **`boot/`** — NASM bootloader and early-boot C.

**Dependencies** follow the general DAG rule (see `PRINCIPLES.md` §3).
Notable constraints:

- `kernel/sec/` may depend on `kernel/sched/` but not vice versa.
- `kernel/svc/` sits above `kernel/sched/` and `kernel/sec/`.
- `kernel/drivers/` must not depend on higher-level subsystems. A
  driver that needs a higher-level service does so via a callback
  registered by that service, not by including its header.

---

## 4. Conventions established in existing code

### 4.1 PID table

- `s_pid_ht[65536]` is the PID hash table, providing O(1) lookup.
- File-static (`s_` prefix), consistent with the file-static naming
  rule.
- Do not add a parallel lookup structure without justifying why the
  existing hash table isn't sufficient.

### 4.2 Paths

- `KPATH_MAX` is the unified kernel path-length constant. **Do not
  introduce a second path-length constant.** If something needs to
  differ, it needs a distinct semantic name, not a different value for
  "path length."

### 4.3 User/kernel boundary

- User addresses are always validated with `copy_from_user` /
  `copy_to_user`. Never dereference a user pointer directly.
- The current working directory is **heap-allocated** per task. Do not
  assume it's inline.

### 4.4 Spawn attributes

- `spawn_attr_t` carries uid, gid, pledge, and unveil inheritance.
- The inline unveil array size is fixed and load-bearing (see §2).
- New attributes that need to be inherited across `spawn` go here.
  Don't invent a parallel mechanism.

### 4.5 Service manager (`svcmgr`)

- Services start in **DAG-ordered** sequence. Dependencies are declared
  in the service manifest; `svcmgr` topologically sorts.
- Do not add imperative "start X before Y" logic. Express it as a
  dependency in the manifest.

### 4.6 IPC / signals

- SIGPIPE delivery lives in `pipe.c`. Ongoing work in this area may
  exist at any given time — check git status before starting changes
  here.

---

## 5. Tooling

### 5.1 Claude Code configuration

- Auto-compaction is **disabled** via `settings.json`. Manual context
  management is the intended workflow.
- Do not re-enable auto-compaction as part of an unrelated change.

### 5.2 CLAUDE.md

- Agent constraints live in `CLAUDE.md` at the repository root.
- This file (`MAKAOS.md`) is the human-and-agent-readable
  project-specific rulebook. `CLAUDE.md` is the agent-prompt-specific
  version of a subset of these rules, optimized for agent behavior.
- When the two conflict, `CLAUDE.md` wins for agent behavior, but the
  conflict itself is a bug — fix both.

### 5.3 Running on hardware

- Primary target: x86_64, Intel Q35 chipset in bare-metal and
  virtualized environments.
- Development environment: Kali Linux host, QEMU for iteration.

---

## 6. Contributing changes

### 6.1 Before a change

1. Identify the subsystem(s) affected.
2. Check §2 for any do-not-touch flags.
3. Read the current state of the file(s) in full — not just the
   function being changed.
4. Check git log for recent activity in the same area to avoid
   colliding with in-progress work.

### 6.2 During a change

- Follow `CODE_STYLE.md` and `PRINCIPLES.md`.
- Keep the change atomic — one logical change per commit.
- Build with `bash build.sh` and verify it boots before committing.

### 6.3 After a change

- Run the in-kernel self-tests (they run at boot in debug builds).
- Run integration tests for any subsystem whose public API was touched.
- If a hot path was modified, benchmark before and after. Claim "no
  regression" only after measuring.

---

## 7. When this file is wrong

This file describes the project as it stands. If you discover a rule
here that no longer matches the code (e.g., a subsystem was renamed, a
constant was replaced), **update this file as part of the same change**
that caused the divergence. Stale project-specific rules are worse
than no project-specific rules — they actively mislead.
