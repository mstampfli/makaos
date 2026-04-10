#include "ksec.h"
#include "sched.h"
#include "process.h"
#include "kheap.h"
#include "errno.h"
#include "vfs.h"

// ── Global agent state ────────────────────────────────────────────────────
//
// Only one policy agent may be registered.  Registered at boot by the ksec
// daemon (uid=0) via sys_register_policy_agent().  After registration the
// kernel starts a dedicated reader thread (ksec_reader_thread) that sits in
// a blocking read loop on the agent's response pipe and wakes waiting tasks.

static vfs_file_t* s_agent_read  = NULL;  // kernel reads responses from here
static vfs_file_t* s_agent_write = NULL;  // kernel writes requests here
static int         s_agent_ready = 0;

// Monotonic sequence counter.
static volatile uint32_t s_seq = 0;

// Pending request slot — one outstanding request at a time.
// (Single-CPU, cooperative: only one task can be sleeping in ksec_request
// at a time.  Multi-CPU extension: replace with a hash-table keyed on seq.)
static struct {
    uint32_t       seq;
    ksec_response_t resp;
    task_t*        waiter;    // task sleeping waiting for this response
    int            done;      // 1 = response received
} s_pending = {0, {0}, NULL, 0};

// ── ksec_register_agent ───────────────────────────────────────────────────

int ksec_register_agent(vfs_file_t* read_pipe, vfs_file_t* write_pipe) {
    if (!read_pipe || !write_pipe) return -EINVAL;
    if (s_agent_ready)            return -EBUSY;   // already registered

    // Caller credential check done in the syscall handler before calling here.

    s_agent_read  = read_pipe;
    s_agent_write = write_pipe;
    s_agent_ready = 1;
    return 0;
}

// ── ksec_agent_present ────────────────────────────────────────────────────

int ksec_agent_present(void) {
    return s_agent_ready;
}

// ── ksec_next_seq ─────────────────────────────────────────────────────────

uint32_t ksec_next_seq(void) {
    return ++s_seq;   // pre-increment: seq 0 is never used
}

// ── ksec_request ─────────────────────────────────────────────────────────
//
// Sends `req` to the policy agent and blocks the calling task until a
// matching response arrives.  The ksec_reader_thread() reads responses
// from the agent pipe and wakes the waiting task.
//
// Returns 0 and populates `resp` on success.
// Returns -EAGAIN if no agent is registered (caller should apply fallback).

int ksec_request(const ksec_request_t* req, ksec_response_t* resp) {
    int64_t written;

    if (!req || !resp) return -EINVAL;
    if (!s_agent_ready) return -EAGAIN;

    // Populate the pending slot.
    s_pending.seq    = req->seq;
    s_pending.waiter = g_current;
    s_pending.done   = 0;

    // Write request to the agent.  The write_pipe is a kernel pipe — this
    // must not block (the ksec reader thread is always running and draining
    // the pipe before it fills).
    written = vfs_write(s_agent_write, req, sizeof(ksec_request_t));
    if (written != (int64_t)sizeof(ksec_request_t)) {
        s_pending.waiter = NULL;
        return -EIO;
    }

    // Sleep until the reader thread wakes us.
    while (!s_pending.done) {
        sched_sleep();
        // After waking: check done again (spurious wakeups are safe here —
        // sched_sleep only returns when sched_wake is called by reader thread,
        // so in practice done will be 1, but defensive loop is correct).
    }

    *resp = s_pending.resp;
    s_pending.waiter = NULL;
    return 0;
}

// ── ksec_notify ───────────────────────────────────────────────────────────
//
// Fire-and-forget: write a notification to the agent, no response expected.

void ksec_notify(const ksec_request_t* req) {
    if (!req || !s_agent_ready) return;
    // Best-effort: if the write fails (pipe full), the notification is lost.
    // ksec performs boot-time reconciliation so occasional lost notifications
    // are harmless — the boot scan will re-sync any divergence.
    vfs_write(s_agent_write, req, sizeof(ksec_request_t));
}

// ── ksec_reader_thread ────────────────────────────────────────────────────
//
// Runs as a kernel thread (task_create_kthread).  Blocks in vfs_read on the
// agent read pipe waiting for responses.  On receipt, matches the sequence
// number, copies the response, and wakes the waiting task.
//
// This thread never exits.

void ksec_reader_thread(void) {
    ksec_response_t resp;
    int64_t n;

    for (;;) {
        if (!s_agent_ready) {
            sched_yield();
            continue;
        }

        // Blocking read from agent pipe — vfs_read blocks until data arrives.
        n = vfs_read(s_agent_read, &resp, sizeof(ksec_response_t));
        if (n != (int64_t)sizeof(ksec_response_t)) {
            // Short read or error: agent may have died.  Yield and retry.
            sched_yield();
            continue;
        }

        // Match sequence number.
        if (s_pending.waiter && s_pending.seq == resp.seq && !s_pending.done) {
            s_pending.resp = resp;
            s_pending.done = 1;
            sched_wake(s_pending.waiter);
        }
        // Unmatched response: stale, discard silently.
    }
}
