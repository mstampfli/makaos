# ksec — Kore Security Policy Daemon

## Overview

ksec is the userland policy engine for the Kore security model. It runs as
uid=0, started by init before any unprivileged process, and registers itself
as the kernel's single policy agent via `sys_register_policy_agent()`. After
registration the kernel blocks on ksec for every privilege escalation decision.

The kernel enforces mechanism (fd rights, setuid exec, ACL checks). ksec
enforces policy (who is allowed to escalate, to what, under what conditions).
Policy changes never require a kernel recompile or reboot — restart ksec.

---

## Startup Sequence

```
init forks ksec
  ksec opens two pipes: req_pipe[2], resp_pipe[2]
  ksec calls sys_register_policy_agent(resp_pipe[0], req_pipe[1])
    kernel stores resp_pipe[0] as "kernel reads responses here"
    kernel stores req_pipe[1] as "kernel writes requests here"
    kernel spawns ksec_reader_thread (reads from req_pipe[0] — wait, see note below)
  ksec reads from req_pipe[0] in its main loop
  ksec writes to resp_pipe[1] in its main loop
```

**Pipe direction (critical — easy to get backwards):**

```
kernel → ksec (requests):
  kernel writes to req_write_fd
  ksec reads from req_read_fd
  → register_policy_agent(resp_read_fd, req_write_fd)
    arg1 = fd kernel reads responses from  = resp_read_fd (ksec writes here)
    arg2 = fd kernel writes requests to    = req_write_fd (ksec reads the other end)

ksec → kernel (responses):
  ksec writes to resp_write_fd
  kernel reads from resp_read_fd
```

So ksec creates:
```c
int req_pipe[2];   // req_pipe[0] = ksec reads, req_pipe[1] = kernel writes
int resp_pipe[2];  // resp_pipe[0] = kernel reads, resp_pipe[1] = ksec writes
pipe(req_pipe);
pipe(resp_pipe);
register_policy_agent(resp_pipe[0], req_pipe[1]);
// ksec loop: read(req_pipe[0], ...), write(resp_pipe[1], ...)
```

The kernel's `ksec_reader_thread` reads responses from `resp_pipe[0]` and wakes
the sleeping task. The kernel writes requests to `req_pipe[1]`.

---

## Wire Protocol

Both sides use fixed-size packed structs. No framing needed — each read/write
is exactly sizeof(ksec_request_t) or sizeof(ksec_response_t).

### ksec_request_t (kernel → ksec)

```c
typedef struct __attribute__((packed)) {
    uint32_t seq;           // monotonic sequence number; response must echo this
    uint8_t  op;            // KSEC_OP_* opcode
    uint8_t  _pad[3];

    uint32_t caller_pid;    // pid of the requesting process
    uint32_t caller_uid;    // caller's effective uid
    uint32_t caller_gid;    // caller's effective gid

    uint32_t inode;         // inode number (EXEC_SETUID, SETUID_BIT, CLRUID_BIT)
    uint32_t dev;           // device number (same)
    uint32_t target_uid;    // requested uid (SETEUID, SETUID)
    char     resource[128]; // path (RESOURCE_FD)
    uint32_t file_uid;      // inode owner uid (SETUID_BIT notification)
} ksec_request_t;
```

### ksec_response_t (ksec → kernel)

```c
typedef struct __attribute__((packed)) {
    uint32_t seq;           // must echo request seq exactly
    uint8_t  verdict;       // KSEC_VERDICT_ALLOW / DENY / CHALLENGE
    uint8_t  _pad[3];

    uint32_t granted_euid;  // euid to set on ALLOW for setuid/seteuid ops
    uint32_t granted_egid;  // egid to set (0 = unchanged)
    uint32_t granted_rights;// fd rights bitmask for RESOURCE_FD grants
} ksec_response_t;
```

### Opcodes

| Opcode | Value | Direction | Response needed |
|---|---|---|---|
| `KSEC_OP_EXEC_SETUID` | 1 | kernel→ksec | yes |
| `KSEC_OP_SETEUID` | 2 | kernel→ksec | yes |
| `KSEC_OP_SETUID` | 3 | kernel→ksec | yes |
| `KSEC_OP_RESOURCE_FD` | 4 | kernel→ksec | yes |
| `KSEC_OP_SETUID_BIT` | 5 | kernel→ksec | NO (notification only) |
| `KSEC_OP_CLRUID_BIT` | 6 | ksec→kernel | N/A |

For `KSEC_OP_SETUID_BIT`: kernel sends notification when `chmod u+s` is called
by a root process. ksec MUST NOT write a response — the kernel does not wait for
one. Writing a response for a notification will corrupt the sequence numbering
because the kernel will read it as a response to the next real request.

---

## Policy File: /etc/ksec.policy

Plain text, one rule per line. Comments start with `#`. Loaded at startup;
`ksecctl reload` sends SIGHUP to reload without restart.

### Format

```
# type  path-or-glob       allow=MATCHER   [target=UID]  [auth=MODE]  [flags=FLAGS]
setuid   /bin/passwd        allow=*         target=0      auth=none
setuid   /bin/su            allow=gid=wheel target=0      auth=password
setuid   /bin/ping          allow=*         target=0      auth=none     flags=drop_after_bind
resource /dev/sda           allow=gid=disk                auth=none
resource /dev/kvm           allow=gid=kvm                 auth=none
resource /dev/input/*       allow=gid=input               auth=none
resource /dev/fb0           allow=gid=video               auth=none
```

### Matcher syntax (allow= field)

| Syntax | Meaning |
|---|---|
| `*` | Any caller |
| `uid=N` | Caller's euid == N |
| `gid=NAME` | Caller's egid == NAME or NAME in supplemental groups |
| `uid=N,gid=NAME` | Both must match |

### auth= values

| Value | Meaning |
|---|---|
| `none` | Grant immediately if allow= matches |
| `password` | ksec challenges caller for password before granting |
| `timed` | Grant without password but only within a time window (like sudo -v) |

### flags= values (comma-separated)

| Flag | Meaning |
|---|---|
| `drop_after_bind` | After exec, reduce euid back to ruid after first bind() |
| `exec_only` | Prevent the binary from passing elevated euid to children |
| `log` | Always log this access even if auth=none |

### Path matching

- Exact path: `/bin/passwd` — matches exactly that path
- Glob: `/dev/input/*` — `*` matches any sequence including `/`
- Inode-keyed entries (auto-created by SETUID_BIT notification): path is
  annotation only; matching uses device+inode number. Immune to renames,
  hardlinks, and bind mounts.

### Drop-in directory

```
/etc/ksec.policy          ← base file, admin-managed
/etc/ksec.policy.d/       ← drop-in directory
    passwd.policy         ← installed by passwd package
    openssh.policy        ← installed by openssh package
```

ksec loads base file first, then all `*.policy` files in the drop-in directory
alphabetically. Package installs write their own file; package removal deletes it.
Admin never edits the base file for normal software.

---

## Two-Way Invariant: setuid bit ⟺ ksec entry

The setuid bit on an inode and a ksec policy entry for that inode must always
agree. ksec enforces this invariant via:

1. **chmod u+s (by root)** → kernel sends `KSEC_OP_SETUID_BIT` notification →
   ksec auto-creates entry → logs it
2. **ksecctl revoke /bin/foo** → ksec deletes entry → sends `KSEC_OP_CLRUID_BIT`
   to kernel → kernel calls `ext2_chmod(inode, mode & ~S_ISUID)` → bit cleared
3. **Boot reconciliation** → ksec scans all inodes with setuid bit set via
   `/proc/setuid_inodes` (a new kernel interface needed), cross-references with
   policy table, and either auto-creates missing entries (with a loud warning) or
   sends CLRUID_BIT for orphaned bits

### Package manager integration

The package manager (running as uid=0):
1. Extracts archive, **strips all setuid bits from extracted files**
2. Reads package manifest for explicit setuid declarations
3. For each declared setuid binary:
   - Calls `chmod u+s /bin/foo` → triggers kernel notification → ksec auto-creates
   - Calls `ksecctl policy /bin/foo allow=MATCHER auth=MODE` to set policy details
4. On package removal: `ksecctl revoke /bin/foo` → clears entry and bit

### nosuid mounts

Filesystems mounted with `nosuid` (default for removable media, loop mounts,
network mounts) have setuid bits ignored by the kernel exec path entirely.
ksec is not consulted — the kernel returns the exec without escalation.
This closes the external-source attack vector: malicious archives with pre-set
bits are inert on nosuid mounts.

---

## Privilege Escalation via /run/ksec Socket

For privileged resource fd hand-off (not setuid exec), processes connect to
ksec via a Unix socket at `/run/ksec`.

### Protocol (over AF_UNIX SOCK_SEQPACKET)

```
Client → ksec: ksec_client_req_t {
    uint8_t  op;         // KSEC_CLIENT_WANT_FD
    char     resource[128]; // e.g. "/dev/sda"
    uint32_t want_rights;   // RIGHT_READ | RIGHT_WRITE etc.
}

ksec → client: ksec_client_resp_t {
    uint8_t  verdict;    // ALLOW / DENY / CHALLENGE
    uint32_t granted_rights; // may be less than requested
    // If ALLOW: SCM_RIGHTS ancdata contains the fd with granted_rights stamped
}
```

ksec verifies caller identity via `SO_PEERCRED` (kernel fills this, cannot be
spoofed by the client). Policy lookup uses caller's pid/uid/gid from peercred.

On ALLOW: ksec opens the resource itself (it holds the privileged fd), stamps
`granted_rights` on a dup, and sends it via `SCM_RIGHTS` ancillary data.
The kernel enforces `granted_rights` on every subsequent operation by the client.

On CHALLENGE (auth=password): ksec responds with CHALLENGE verdict. Client must
open a pinentry helper (or prompt on its tty), hash the password, and send a
`KSEC_CLIENT_CHALLENGE_RESP` message. ksec verifies against `/etc/shadow`.

---

## ksecctl — Admin Tool

```
ksecctl list                        # print current policy table
ksecctl allow /bin/foo allow=* target=0 auth=none
ksecctl revoke /bin/foo             # delete entry + clear setuid bit
ksecctl reload                      # send SIGHUP to ksec (reload policy file)
ksecctl status                      # show ksec daemon status + agent registration
ksecctl audit                       # show recent grant/deny log
```

`ksecctl` communicates with ksec via `/run/ksec` using a separate command channel
(distinguished from the resource-request channel by a different message type).

---

## Security Properties

- **No ambient setuid**: a binary with the setuid bit but no ksec entry execs
  without escalation. The bit alone is not sufficient.
- **No external-source escalation**: files arriving with pre-set bits from
  tarballs, disk images, or network mounts are inert (stripped on extract, or
  nosuid mount).
- **Audit trail**: every ALLOW and DENY is logged by ksec. The log is the
  authoritative record — no separate audit daemon needed.
- **Policy without kernel changes**: ksec policy can be updated and reloaded
  without recompiling or rebooting the kernel.
- **Single point of policy**: no sudoers + PAM + setuid bits as three separate
  systems. One file, one tool, one daemon.
- **Kernel enforcement is final**: the kernel checks fd rights on every
  syscall. ksec's ALLOW just causes the kernel to set the euid or hand out an
  fd. The kernel never trusts userland to have correctly enforced anything.
