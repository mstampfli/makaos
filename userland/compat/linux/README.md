# userland/compat/linux — Linux-ABI compatibility shims

This is the **single home** for Linux-library compatibility shims. MakaOS is a
native kernel with its own ABI (its own syscall numbers, and free to design its
own subsystem surfaces); Linux software is ported by **recompiling from source**
against a Linux-shaped libc. Where a Linux *library* API is expected that MakaOS
does not provide natively (libseat, libudev, mtdev, iconv, ...), the shim that
provides that API lives **here**, in one place — never injected per-port.

## Rules

1. **One home, not per-port.** A port script (`scripts/port-*.sh`) does BUILD
   GLUE only — flags, cross-files, header-path patches, compile/link. It MUST
   NOT heredoc or `sed`-inject functionality. If a port needs an API, the shim
   for it is a real, reviewable, git-tracked source file under this directory,
   and the port script just compiles/links it.

2. **General, not app-tailored.** A shim implements the **general Linux library
   surface** so *any* consumer works — not the minimal subset one app happens to
   call. Under-implementing to "just what wlroots needs" is the anti-pattern.

3. **Backed by native MakaOS features, not faked.** A shim translates the Linux
   API to a real native MakaOS mechanism (a kernel subsystem, a syscall). When
   the native feature does not exist yet, the shim may be a **documented stub —
   but the stub lives HERE**, and the missing native feature is recorded in
   `docs/SCALABILITY_DEBT.md` with the path to the real implementation. As the
   native feature lands, the shim becomes a thin real client without moving.

4. **This is not a Linux binary personality.** These shims are for *source*
   ports (recompiled against MakaOS's libc). Running unmodified Linux binaries
   would be a separate syscall-translation personality — explicitly out of scope
   here.

## Layout

```
compat/linux/<library>/<library>.c   # the shim implementation (source of truth)
compat/linux/<library>/<library>.h   # the shim's public header (if it ships one)
```

## Current shims and their native backing

| shim | Linux API | native backing (target) | debt |
|------|-----------|-------------------------|------|
| libseat | seat/session/device-access | capability + session subsystem | ledger #1 |
| libudev | device discovery + hotplug | kernel device registry | ledger #2 |
| libevdev | evdev event decode | kernel evdev (real) | — |
| mtdev | MT protocol A→B | (kernel emits B natively) | ledger #10 |
| libiconv | charset conversion | real converter / port | ledger #15 |

See `docs/SCALABILITY_DEBT.md` for each stub's path to a real, general
implementation. The convergent native features are a **device registry**
(libudev + libdrm enumeration) and a **capability/session broker** (libseat).
