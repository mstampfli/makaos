# MakaOS Security v2 — Capability-First Architecture

**Status:** Forward-looking design.  Do NOT implement yet.  The current
system (v1) stays in place — current `ksec` (KSEC.md) continues to
serve as the setuid/escalation arbiter until the kernel is stable.
The refactor to v2 happens only after the kernel hits a self-declared
stable milestone (no regressions across 10+ boot stress cycles, all
current ports continue to work).

This document captures the decisions from the capability-first design
discussion so they don't need to be re-argued when the refactor window
opens.

---

## 1. Thesis

MakaOS v2's security story is: **per-app isolation by default, with
capabilities as the single authoritative permission model**.  UIDs
exist at the POSIX syscall ABI only — they are a compatibility label,
not a security mechanism.  One compromised app does not leak state
belonging to any other app.

The v2 architecture is what justifies MakaOS's existence as a project
distinct from "yet another from-scratch Linux-shaped OS."  Without
this property, there is no compelling selling point.  With it, MakaOS
is the first desktop/workstation OS where the "Android model" (per-app
UIDs, manifest-scoped capabilities, no setuid) is the default rather
than a retrofit.

---

## 2. Core design decisions

### 2.1  UID as compatibility label, `cap_ctx` as authority

Every task carries two identity fields, separated by purpose:

```c
typedef struct {
    uint32_t    uid;        // POSIX-surface only; never read by security path
    cap_ctx_t*  caps;       // authoritative security identity
} task_identity_t;
```

- `uid`: returned by `getuid()`, stored in `stat().st_uid`, written to
  inode on-disk, passed to POSIX callers.  Never consulted for any
  access-control decision.
- `caps`: the pointer used on every syscall entry to check whether
  the requested operation is permitted.  Resolved once at process
  creation; not re-resolved per syscall.

**One writer rule:** `task_set_identity(task, uid, caps)` is the only
function that modifies either field.  Grep should return exactly one
writer site.  This prevents `uid`/`caps` desync bugs which are the
Linux `struct cred` CVE class.

**Why split them:** if kernel memory corruption lets an attacker
overwrite `uid` (4 bytes, public value of any installed app), that's
cosmetic — `getuid()` lies, caps are unaffected.  To actually escalate
they must also overwrite `caps` (8-byte pointer into randomized kernel
heap, requires info leak).  Lookup-key design (caps = sessiond(uid))
makes a single `uid` overwrite fully escalatory; compat-only design
requires two independent primitives.  Defense in depth for one extra
field.

### 2.2  No setuid, in any form

The following are illegal in v2, enforced in kernel:

- `S_ISUID` / `S_ISGID` bits on inodes: **not read by the exec path.**
  Deleted from `exec()` permission logic entirely.  A file with the
  bit set is just a file.
- `setuid()` / `seteuid()` / `setresuid()` / `setfsuid()` syscalls:
  return `-EPERM` unconditionally, or don't exist.
- No `fsuid`, no saved UID, no supplementary groups as cap carriers.
  `task->uid` is write-once at process creation; no mutation API.
- No filesystem capabilities (`setcap` xattrs).  The exec path never
  reads xattrs for capability grants.
- No `capable(CAP_*)` scattered check pattern.  Security gating is one
  function called at syscall entry.

The `/etc/shadow` + `/etc/passwd` path disappears as the root of
authority.  Those files may exist as POSIX-compat stubs (one entry per
installed app, so `getpwuid()` resolves names), but they carry no
authority.

### 2.3  Per-app UIDs minted at install

- Package installer mints a fresh unique UID per app install
  (monotonic counter from `10000+`, or random — uniqueness is the
  requirement).
- Uninstalling releases the UID; it is not reused within one boot
  session (prevents stale-fd confusion).
- System services each have their own fixed UID (`1 = sessiond`,
  `2 = network broker`, `3 = audio broker`, etc.), each with the
  minimum capability set their role requires.
- **There is no uid=0 "can do anything" account.**  Operations that
  require broad privilege go through specific system services with
  specific capabilities each.  Package installation is one such
  service.  Nothing has cap-everything.

Login authenticates the human to the device, but doesn't assign a
"your" UID.  Logged-in human runs apps; apps run as their install
UIDs; the human's identity is only checked at the login-session
boundary.

### 2.4  Per-app filesystem namespaces

Each app sees a private filesystem view:

- `/home/<app-install-id>/`: the app's private storage.  Owned
  `<install-uid>:<install-uid>`, mode `0700`.  No other app can see
  this path exists.
- `/tmp`: per-app view, private per process tree.  Not shared.
- `/dev`: per-app view, only contains device nodes the manifest grants
  (`/dev/dri/card0` only if the app has display capability, etc.).
- `/etc`: mostly read-only, globally visible (config files, timezone,
  manifest-derived stubs).  Apps can't write.
- System paths: granted by manifest.  App without camera capability
  cannot name `/dev/video0` — the file doesn't exist in its view.

**Sharing between apps is explicit.**  Via broker-delivered fds
(portals), SCM_RIGHTS hand-off, or explicit manifest declarations for
shared directories (`~/Documents/shared/`).  Never via ambient
filesystem visibility.

Implementation sketch: each app's process tree holds its own mount
namespace + overlayfs-style view.  The namespace is configured once
at app launch by the launcher service, based on the app's manifest.

### 2.5  One chokepoint for capability enforcement

All syscall entries pass through a single capability check gate:

```c
syscall_entry(nr, a1, a2, ...):
    cap_ctx_t* caps = current->id.caps;
    check_operation(caps, nr, a1, a2, ...);   // gate here
    dispatch(nr, a1, a2, ...);
```

No scattered `if (!capable(CAP_X)) return -EPERM;` sites inside
drivers or subsystem handlers.  The gate is the contract: if the
operation reaches `dispatch()`, caps have already been verified.
This eliminates the Linux "missed capability check in a driver" CVE
class.

Fine-grained ops that need finer granularity (e.g., specific ioctls
on a permitted device) are modeled as sub-capabilities attached to
the same `cap_ctx` — still one check surface, just richer.

---

## 3. ksec v2

### 3.1  Role change

**ksec v1:** escalation arbiter.  Decides who may `setuid()` or
receive privileged fds.  Works within the Linux-shaped authority
model.

**ksec v2:** capability broker.  No escalation; caps are installed
with the app and never change during process lifetime (except via
voluntary `pledge()`-style revocation).  ksec brokers runtime
resource-fd grants, user-consent flows, and manifest registration.

### 3.2  What's kept from v1

The structural bones transfer cleanly:

- Userland daemon running as a dedicated UID, kernel enforces mechanism.
- Pipe-based kernel↔ksec protocol with seq numbers, fixed-size packed
  structs, one request/response per pipe write.
- `/run/ksec` Unix socket for client requests, `SO_PEERCRED` for
  kernel-verified caller identification.
- `SCM_RIGHTS` fd hand-off with `granted_rights` bitmask stamped by
  the broker.  Kernel enforces the rights on every subsequent syscall
  against that fd.
- Drop-in policy directory (`/etc/ksec.policy.d/`) populated by
  package installs.
- `ksecctl` admin UX + SIGHUP reload.
- Auth modes `none` / `password` / `timed` for runtime consent.

### 3.3  What's deleted from v1

The escalation semantics are removed:

- `KSEC_OP_EXEC_SETUID`, `KSEC_OP_SETEUID`, `KSEC_OP_SETUID`: gone.
  No uid transitions exist to arbitrate.
- `KSEC_OP_SETUID_BIT`, `KSEC_OP_CLRUID_BIT`: gone.  The setuid bit on
  inodes carries no meaning; there is no two-way invariant to
  maintain.  Boot reconciliation path removed.
- `granted_euid`, `granted_egid` response fields: removed.
- `/etc/ksec.policy` grammar keyed on `setuid /bin/foo`: removed.
- `target_uid`, `file_uid`, `gid` request fields: removed.
- `target=0 auth=password`-style "become root" flows: removed.  There
  is no root to become.
- `nosuid` mount flag: vacuous since the kernel never reads the bit.
  Remove the flag; document that MakaOS mounts always ignore setuid
  bits.

### 3.4  What's added in v2

#### 3.4.1  Manifest registration

Package installer calls ksec to register an app at install time:

```
KSEC_OP_INSTALL_APP:
    caller:       package manager (verified by peercred UID)
    app_name:     "com.mozilla.firefox"
    install_uid:  <minted by ksec>
    manifest:     full capability declaration (see 3.4.2)
    → response:   install_uid, success/fail
```

Uninstall:

```
KSEC_OP_UNINSTALL_APP:
    caller:       package manager
    install_uid:  <to remove>
    → response:   success/fail (after cap revocation + filesystem cleanup)
```

#### 3.4.2  Manifest grammar

Plain text, per-app stanza.  Each app ships its own file in the
drop-in directory:

```
app com.mozilla.firefox install=47:
    home       /home/firefox-47/    rw
    resource   /dev/dri/card0       mmap
    broker     notifications        talk
    broker     portal.file-picker   request
    broker     audio                output
    network    outbound             allow
    network    inbound              deny

app org.kde.okular install=48:
    home       /home/okular-48/     rw
    resource   /dev/dri/card0       mmap
    broker     portal.file-picker   request
    network    outbound             deny
```

Grammar rules:
- `install=N`: UID bound to this stanza (minted at registration, not
  specified by the app).
- `home <path> <rights>`: app's private home with declared rights.
- `resource <path> <rights>`: specific privileged resource access.
- `broker <name> <op>`: permission to call this op on this service.
- `network <direction> <decision>`: default decision for outbound /
  inbound network operations.

No prefix globs on `broker` names (security-relevant; exact match
only).  No wildcards on `resource` paths by default; a narrow class
may accept `resource /dev/input/event* read` for keyboard/mouse
grouping.

#### 3.4.3  Broker connection request

`KSEC_OP_BROKER_CONNECT`: "give me a connected fd to service X."
ksec consults the calling app's manifest, opens a socket to the
target broker on the app's behalf, hands the fd back via SCM_RIGHTS
with rights stamped per manifest.  The app now has a direct connection
to the broker; ksec is out of the hot path.

#### 3.4.4  User-consent capability grants

Some capabilities are manifest-optional but require user consent at
runtime (camera, screen capture, clipboard read).  The manifest
declares the *possibility*, ksec prompts the user at request time, a
grant creates a time-limited sub-capability attached to `cap_ctx`.

```
app com.someapp.videocall install=52:
    ...
    consent    camera               user-prompt timeout=1800
    consent    screen-capture       user-prompt
```

The consent prompt is rendered by a trusted pinentry-equivalent
service (its own UID with display + input capabilities).  App sees
only an opaque fd on success or `-EACCES` on denial.

#### 3.4.5  Voluntary capability shedding (`pledge`-style)

An app can voluntarily drop capabilities from its `cap_ctx` at
runtime.  Dropped caps cannot be reacquired.  Purely additive safety
for well-behaved apps (e.g., "after startup I no longer need network;
drop it"); security model doesn't depend on it.

```c
int pledge(const char* promises);   // drops caps not in promise set
int unveil(const char* path, const char* rights);  // narrows fs view
```

These hook into the same `cap_ctx` structure, not a separate system.

---

## 4. D-Bus compat (recap from separate spec discussion)

v2 D-Bus compat uses the **resolver daemon** approach, not the in-
process adapter:

- A small daemon speaks D-Bus wire protocol on a per-app socket,
  handed to the app via SCM_RIGHTS at launch.
- Daemon translates incoming D-Bus messages to native broker calls.
- Manifest-scoped enforcement at the socket boundary.
- Covers libdbus, GDBus, sd-bus, and every other D-Bus client library
  for free (they all emit the same wire protocol).
- D-Bus wire exists only on the per-app socket.  No system bus, no
  ambient authority.

This is "compat plumbing, not architecture" — documented as such so
no one thinks D-Bus is native to MakaOS.

---

## 5. What v2 changes in the kernel

Concrete kernel-side work required for v2:

1. `task_t` gains `task_identity_t` field (uid + caps).  Single writer
   `task_set_identity`.  Remove every other uid write site.
2. `exec()` permission path: delete reads of `S_ISUID`, `S_ISGID`,
   security xattrs.  Exec never changes credentials.
3. `setuid()` / `seteuid()` / `setresuid()` / `setfsuid()` syscalls:
   replace handlers with `return -EPERM`.
4. Remove every `if (uid == inode->i_uid ...)` authorization check
   from VFS.  Replace with `cap_ctx` consult.
5. Syscall entry: single `check_operation(current->id.caps, ...)` call
   gating all dispatch.  Remove scattered capability checks.
6. Per-app mount namespace + overlayfs-equivalent view.  New mount
   namespace per app launch; manifest-driven composition.
7. Filesystem: inode uid field kept (storage), read-returned on
   `stat()`, never consulted for access decisions.
8. `/proc`: per-app view, filtered to own processes.
9. IPC socket namespace: per-app, only socket paths the manifest
   grants are visible.
10. Remove `/etc/shadow` + passwd authority; keep them as compat stubs
    backed by the app registry.

---

## 6. Migration from v1

The transition is disruptive; sequence matters.

1. **Stable kernel gate** (prerequisite).  10+ consecutive stress
   cycles with no regressions.  All currently-ported software
   continues to work.  Before this gate, v1 stays.
2. **Mount namespace infrastructure in kernel.**  Foundational; other
   v2 pieces depend on it.  Can be landed incrementally while v1
   authority model still runs.
3. **Task identity split (uid / caps fields).**  Kernel-only; v1 ksec
   still works because uid still behaves v1-compatibly during
   transition.
4. **`cap_ctx` populated from v1 manifests.**  Translate v1 policy
   entries into equivalent v2 cap contexts.  Both authority paths
   check in parallel temporarily.
5. **Kill setuid path.**  Delete S_ISUID handling from exec.  v1
   setuid binaries stop escalating; rebuild them as system services
   with specific caps.
6. **Kill v1 ksec ops.**  `KSEC_OP_EXEC_SETUID` etc. removed;
   kernel no longer sends them.  Resource-fd path expanded to cover
   all grant scenarios.
7. **Per-app UIDs at install time.**  Package installer mints new
   UIDs; existing installs migrated once.
8. **Per-app filesystem namespaces.**  Filesystem sharing defaults to
   "isolated."  Audit ported software for ambient-path assumptions.
9. **Manifest grammar switch.**  New `/etc/ksec.policy.d/` format.
   v1 format translator available for migration; deprecated within
   one release.
10. **D-Bus resolver daemon deployment.**  Replaces any v1-era
    D-Bus compat (if v1 ever shipped one).

Each step is reversible until step 5; after step 5, rollback requires
rebuilding setuid binaries or restoring the old exec path.

---

## 7. What v2 explicitly does not attempt

- **Pure capability (seL4-style) kernel.**  We keep the POSIX syscall
  surface.  UIDs are storage/pass-through for POSIX callers.  Not
  negotiable without abandoning the port story.
- **Running Linux apps unmodified via namespace trickery alone.**
  Some ports need manifest declarations; the point of the architecture
  is that these declarations exist and are auditable.  "Just works"
  is not a goal when it conflicts with sandboxing.
- **Backward compat with v1 policy files.**  Translation is provided
  once; ongoing v1 syntax support is not.

---

## 8. Selling-point framing

Marketing / external-facing description of v2 (for when it ships):

> MakaOS ships with per-app isolation by default.  Every installed app
> has its own UID, its own filesystem view, and its own set of
> explicitly-granted capabilities.  A compromised app cannot read
> another app's files, cannot enumerate other running processes,
> cannot talk to services it has not been granted access to, and
> cannot escalate its privileges through any file attribute or
> syscall.  There is no setuid.  There is no ambient session bus.
> There is no root account.  Permissions are declared in a human-
> readable manifest at install time, enforced by the kernel, and
> visible in one place.

The thesis: *one compromised app cannot leak anything outside its own
sandbox.*  Android and iOS deliver this on mobile; MakaOS is the
answer for desktop.

---

## 9. Gating note

**v2 starts only after the kernel is self-declared stable.**  "Stable"
means:

- 10+ consecutive boot+stress cycles with no regressions.
- All current boot selftests PASS.
- Hyprland, wlroots, or an equivalent Wayland compositor runs under
  v1 end-to-end without crashes for at least one hour of real use.
- All current Tier 2 / Tier 3 ports continue to compile and run.
- A memory-safety audit has been performed on the kernel's task,
  VFS, and IPC paths.

Before that gate, v1 (current ksec + current UID semantics) is the
production architecture.  v2 work can happen on a branch but does not
land on `main` until the gate is met.

**Rationale:** v2 is a bet-the-farm refactor that touches exec, VFS,
task creation, and every syscall entry.  Attempting it on unstable
foundations guarantees cascading bugs that cannot be distinguished
from pre-existing instability.  First stability; then architecture.
