#pragma once
#include "common.h"
#include "cred.h"
#include "vfs.h"

// ── Kernel↔ksec Policy Channel ───────────────────────────────────────────
//
// ksec is a privileged userland daemon (uid=0, started by init) that holds
// the system policy database.  The kernel communicates with ksec over a
// dedicated synchronous channel established at boot via sys_register_policy_agent().
//
// Protocol:
//   1. ksec calls sys_register_policy_agent(read_fd, write_fd) at startup.
//      Kernel stores these fds.  Only one agent may be registered; only a
//      uid=0 process at boot time may call this (enforced in syscall).
//   2. When the kernel needs an authorisation decision it:
//      a. Writes a ksec_request_t into the write-end of the agent's pipe.
//      b. Sleeps the requesting task (sched_sleep).
//   3. ksec wakes from poll/read on its fd, reads the request, evaluates
//      policy, and writes a ksec_response_t back.
//   4. The kernel wakes the sleeping task, reads the response, and proceeds.
//
// If no policy agent is registered, the kernel denies all requests that
// would require one (setuid exec bit → exec without escalation, i.e. euid
// unchanged, unless the calling process is already root).

// ── Request opcodes ───────────────────────────────────────────────────────
#define KSEC_OP_EXEC_SETUID   1   // process is exec-ing a setuid binary
#define KSEC_OP_SETEUID       2   // seteuid() outside three-slot POSIX rules
#define KSEC_OP_SETUID        3   // setuid() with escalation
#define KSEC_OP_RESOURCE_FD   4   // process requests a privileged resource fd
#define KSEC_OP_SETUID_BIT    5   // chmod u+s was called (notification, no reply needed)
#define KSEC_OP_CLRUID_BIT    6   // ksec wants kernel to clear setuid bit on inode

// ── Verdict codes ─────────────────────────────────────────────────────────
#define KSEC_VERDICT_ALLOW    0
#define KSEC_VERDICT_DENY     1
#define KSEC_VERDICT_CHALLENGE 2  // agent wants auth challenge (password etc.)

// ── Wire structures (both sides must agree on layout) ─────────────────────

typedef struct __attribute__((packed)) {
    uint32_t seq;          // monotonic sequence number; response echoes it
    uint8_t  op;           // KSEC_OP_*
    uint8_t  _pad[3];

    uint32_t caller_pid;
    uint32_t caller_uid;
    uint32_t caller_gid;

    // For EXEC_SETUID / SETUID_BIT / CLRUID_BIT:
    uint32_t inode;        // inode number of the file
    uint32_t dev;          // device number

    // For SETEUID / SETUID:
    uint32_t target_uid;   // requested euid/uid

    // For RESOURCE_FD:
    char     resource[128]; // path to the resource being requested

    // For SETUID_BIT notification:
    uint32_t file_uid;     // owner uid of the file that got u+s set
} ksec_request_t;

typedef struct __attribute__((packed)) {
    uint32_t seq;          // must match request seq
    uint8_t  verdict;      // KSEC_VERDICT_*
    uint8_t  _pad[3];

    // For ALLOW on EXEC_SETUID / SETEUID / SETUID:
    uint32_t granted_euid; // the euid to set (may differ from requested)
    uint32_t granted_egid; // the egid to set (0 = unchanged)

    // For ALLOW on RESOURCE_FD:
    uint32_t granted_rights; // fd rights bitmask (RIGHT_* from rights.h)
} ksec_response_t;

// ── Kernel-side API ───────────────────────────────────────────────────────

// Called from sys_register_policy_agent() — boot-only, uid=0 only.
// Stores the agent's read+write vfs_file_t* for future requests.
// Returns 0 on success, -1 if already registered or caller is not root.
int ksec_register_agent(vfs_file_t* read_pipe, vfs_file_t* write_pipe);

// Returns 1 if a policy agent is registered.
int ksec_agent_present(void);

// Send a request to the policy agent and BLOCK until a response arrives.
// The calling task is put to sleep; the ksec reader thread calls
// ksec_deliver_response() which wakes it.
// Returns 0 and fills `resp` on success; returns -1 if no agent (denied).
int ksec_request(const ksec_request_t* req, ksec_response_t* resp);

// Send a fire-and-forget notification (no response expected).
// Used for KSEC_OP_SETUID_BIT and KSEC_OP_CLRUID_BIT.
void ksec_notify(const ksec_request_t* req);

// Monotonic sequence number allocator.
uint32_t ksec_next_seq(void);

// ── setuid-on-exec ─────────────────────────────────────────────────────────

// Pure decision: should a setuid-on-exec escalation be applied?  FAIL-CLOSED --
// it requires a real escalation request (setuid_uid != 0xFFFFFFFF, set by
// vfs_check_exec), a registered policy agent, a successful round-trip, and an
// explicit ALLOW verdict.  Anything else -> 0 (exec keeps the inherited euid).
// Pure -> unit-tested (ksec_exec_setuid_selftest).
static inline int ksec_exec_setuid_should_apply(uint32_t setuid_uid,
                                                int agent_present,
                                                int ksec_rc, uint8_t verdict) {
    if (setuid_uid == 0xFFFFFFFFu) return 0;   // no setuid bit / no escalation
    if (!agent_present)            return 0;   // no policy agent -> fail closed
    if (ksec_rc != 0)              return 0;   // round-trip failed
    return verdict == KSEC_VERDICT_ALLOW;       // only on explicit ALLOW
}

// Apply a setuid-on-exec escalation to `target`, mediated by the policy agent.
// Called from the exec paths after a setuid binary passed its exec check.
// `setuid_uid` is vfs_check_exec's out value (file owner uid, or 0xFFFFFFFF =
// no escalation).  The kernel NEVER escalates on its own: only an explicit ksec
// ALLOW raises privilege, to the euid/egid ksec grants.  No agent / deny /
// failure -> no change (preserves the fail-closed default).
void ksec_exec_setuid(cred_t* target, uint32_t setuid_uid, uint32_t inode,
                      uint32_t caller_pid, uint32_t caller_uid,
                      uint32_t caller_gid);

// Called from ksec's sys_read loop in kernel — reads response from agent
// pipe and wakes the waiting task.  Called via a dedicated kernel thread
// that sits in a read loop on the agent read_pipe.
void ksec_reader_thread(void);
