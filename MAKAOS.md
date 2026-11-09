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
debugging. The convention is to flag them in-file with a comment
starting `/* DO NOT TOUCH */` and an explanation of why. No such
marker exists in the tree today; the rule applies to any region that
gets one.

**Do not modify a flagged region without explicit permission in the
current session.** Fixes that are load-bearing in the same spirit,
even though unmarked:

- **`spawn_attr_t` carries unveil entries by user pointer + count**,
  not inline. A 64-entry inline array (~16 KB) used to be copied onto
  the 8 KiB kernel stack in `sys_spawn` and corrupted it. Do not move
  the entries back inline; the kernel copies them into a `kmalloc`
  buffer bounded by `SPAWN_UNVEIL_MAX`.
- **`path_split` (`kernel/include/kstr.h`) takes an explicit `cap`**
  and rejects a parent longer than the destination buffer. Unbounded
  copies here overflowed the kernel stack (see
  `ext2_path_split_selftest`).

If a `DO NOT TOUCH` region seems to need modification, **stop and ask**.
The correct response in 99% of cases is "don't." In the 1% case where
it really does need to change, the change should be paired with a
revision of the flag comment explaining the new invariant.

---

## 3. Subsystem boundaries

Current major subsystems and their public headers:

- **`kernel/proc/`** - scheduler (`sched.c`, PID table), processes,
  ELF loader, signals, futex, RCU.
- **`kernel/mm/`** - physical/virtual memory, heap, slab, page cache.
- **`kernel/fs/`** - VFS, dcache, `ext2.c`, FAT32, procfs, virtfs,
  pipes (including SIGPIPE delivery in `pipe.c`).
- **`kernel/security/`** - `pledge`/`unveil`, permissions, credentials,
  `ksec`.
- **`kernel/syscall/`** - syscall dispatch and most syscall bodies.
- **`kernel/io/`** - io_uring, eventfd, signalfd, timerfd.
- **`kernel/net/`** - TCP/IP stack, sockets, UNIX sockets, virtio-net.
- **`kernel/drivers/`** - hardware drivers (audio, input, PCI, storage,
  tty/pty, video/DRM).
- **`kernel/arch/x86_64/`**, **`kernel/time/`**, **`kernel/acpi/`** -
  CPU, interrupts, SMP bring-up, timers, ACPI.
- **`boot/`** - UEFI bootloader (`boot/uefi`, C, built as `BOOTX64.EFI`).
  It is the only boot path.
- **`userland/apps/svcmgr/`** - the service manager runs in userland,
  not in the kernel.

**Dependencies** follow the general DAG rule (see `PRINCIPLES.md` §3).
Intended constraint, not yet fully met:

- `kernel/drivers/` should not depend on higher-level subsystems. A
  driver that needs a higher-level service should get it via a
  callback registered by that service, not by including its header.
  Today several drivers (tty, input, audio, storage, DRM) still
  include `sched.h`/`process.h`/`vfs.h`; do not add new ones.
- `kernel/proc/process.h` embeds the pledge/unveil state in `task_t`,
  so `proc` and `security` are mutually dependent at the header level.

---

## 4. Conventions established in existing code

### 4.1 PID table

- `s_pid_ht` (`kernel/proc/sched.c`) is the PID hash table: open
  addressing with tombstones, grown at 75% load, RCU-published so
  `pid_ht_find` takes no lock. Writers serialize on `s_pid_ht_lock`.
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

- `spawn_attr_t` (`kernel/syscall/syscall.h`) carries uid/gid, umask,
  pledge, and unveil inheritance, selected by `SPAWN_ATTR_*` flags.
- Unveil entries are passed by user pointer + count, never inline
  (see §2).
- New attributes that need to be inherited across `spawn` go here.
  Don't invent a parallel mechanism.

### 4.5 Service manager (`svcmgr`)

- Services start in **DAG-ordered** sequence. Dependencies are declared
  with `after=` in `/etc/services/*.svc`; `svcmgr`
  (`userland/apps/svcmgr/svcmgr.c`) topologically sorts.
- Do not add imperative "start X before Y" logic. Express it as a
  dependency in the manifest.

### 4.6 IPC / signals

- SIGPIPE delivery lives in `kernel/fs/pipe.c`. Ongoing work in this area may
  exist at any given time — check git status before starting changes
  here.

---

## 5. Tooling

### 5.1 Claude Code configuration

- Auto-compaction is **disabled** in the developer's local Claude Code
  `settings.json` (not tracked in this repo). Manual context
  management is the intended workflow.
- Do not re-enable auto-compaction as part of an unrelated change.

### 5.2 Agent rules

- There is no `CLAUDE.md` in this repo. Agent constraints live in
  `DO_NOT_TOUCH.md` at the repository root.
- This file (`MAKAOS.md`) is the human-and-agent-readable
  project-specific rulebook; `DO_NOT_TOUCH.md` is the agent-specific
  subset, optimized for agent behavior.
- When the two conflict, the conflict itself is a bug - fix both.

### 5.3 Running on hardware

- Target: x86_64 with UEFI firmware. The run scripts boot it in
  QEMU/KVM with OVMF on QEMU's default `pc` machine (AHCI disk,
  virtio-gpu, virtio-net). No script targets real hardware, and
  bare-metal boots are not regularly tested.
- Development environment: WSL2 Ubuntu 24.04 with QEMU/KVM (the fresh-clone
  build was verified there on 2026-11-08); any x86-64 Linux with KVM should do.
  KVM is required to boot, because the kernel needs x2APIC. See
  `docs/BUILDING.md`.

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

- Run the in-kernel self-tests (opt-in: `SELFTESTS=1 bash build.sh`
  compiles them in with `-DMAKAOS_BOOT_SELFTESTS`; they run at boot).
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
