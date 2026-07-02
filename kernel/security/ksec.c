#include "ksec.h"
#include "kprintf.h"   // kprintf_atomic (locked whole-line output for selftest result lines)
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

// Monotonic sequence counter (atomic: concurrent setuid callers must get
// distinct seqs, else two slots collide and the reader matches the wrong one).
static uint32_t s_seq = 0;

// In-flight request slots, keyed by sequence number.  Replaces the old single
// global slot: on SMP a second concurrent caller (sys_setuid / sys_seteuid /
// setuid-exec) overwrote the one slot -> the reader could sched_wake a task_t*
// that had already returned + exited (a UAF), AND a "behind" caller read the
// other caller's verdict (cross-talk -> privilege escalation in the gate).
// Each ksec_request now claims its OWN slot under s_ksec_lock; the reader
// matches a response to its slot by seq.  seq == 0 marks a free slot
// (ksec_next_seq never returns 0).  This is the "hash-table keyed on seq" the
// old comment prescribed for the multi-CPU case.
#define KSEC_MAX_INFLIGHT 64

typedef struct {
    uint32_t        seq;       // 0 == free slot
    ksec_response_t resp;
    task_t*         waiter;    // task sleeping for this response
    int             done;      // 1 == response received
} ksec_slot_t;

static spinlock_t  s_ksec_lock = SPINLOCK_INIT;
static ksec_slot_t s_slots[KSEC_MAX_INFLIGHT];

// Find the in-flight slot for `seq` (NULL if none).  Caller MUST hold s_ksec_lock.
static ksec_slot_t* ksec_slot_find(uint32_t seq) {
    if (!seq) return NULL;
    for (int i = 0; i < KSEC_MAX_INFLIGHT; i++)
        if (s_slots[i].seq == seq) return &s_slots[i];
    return NULL;
}

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
    uint32_t s;
    do { s = __atomic_add_fetch(&s_seq, 1u, __ATOMIC_RELAXED); } while (s == 0);
    return s;   // never 0 (the free-slot marker); skips 0 on a u32 wrap
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
    if (!req || !resp) return -EINVAL;
    if (!s_agent_ready) return -EAGAIN;

    // Claim a per-seq slot under the lock.  Fail CLOSED if all are in use --
    // denying a setuid is safe; sharing one slot is the bug we are fixing.
    spin_lock(&s_ksec_lock);
    ksec_slot_t* slot = NULL;
    for (int i = 0; i < KSEC_MAX_INFLIGHT; i++)
        if (s_slots[i].seq == 0) { slot = &s_slots[i]; break; }
    if (!slot) { spin_unlock(&s_ksec_lock); return -EAGAIN; }
    slot->seq    = req->seq;
    slot->waiter = g_current;
    slot->done   = 0;
    spin_unlock(&s_ksec_lock);

    // Write the request to the agent.  The write_pipe is a kernel pipe -- this
    // must not block (the reader thread drains the response pipe continuously).
    int64_t written = vfs_write(s_agent_write, req, sizeof(ksec_request_t));
    if (written != (int64_t)sizeof(ksec_request_t)) {
        spin_lock(&s_ksec_lock);
        slot->seq = 0; slot->waiter = NULL;
        spin_unlock(&s_ksec_lock);
        return -EIO;
    }

    // Sleep until the reader sets our slot's done.  The lock is DROPPED around
    // sched_sleep (never held across a sleep).  The reader sets done + wakes us
    // UNDER the lock, and we free our slot UNDER the lock after waking -- so the
    // reader never wakes a task that has already freed its slot or returned.
    // Signal-interruptible: a killed/signalled setuid frees its slot and returns
    // rather than leaving a dangling waiter (a UAF wake) or hanging forever.
    spin_lock(&s_ksec_lock);
    while (!slot->done) {
        spin_unlock(&s_ksec_lock);
        sched_sleep();
        if (signal_has_actionable(&g_current->sigstate)) {
            spin_lock(&s_ksec_lock);
            slot->seq = 0; slot->waiter = NULL;
            spin_unlock(&s_ksec_lock);
            return -EINTR;
        }
        spin_lock(&s_ksec_lock);
    }
    *resp = slot->resp;
    slot->seq = 0; slot->waiter = NULL;
    spin_unlock(&s_ksec_lock);
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

        // Match the response to its in-flight slot by seq, UNDER the lock.
        // Set done + wake the waiter under the lock: the requester frees its
        // slot only under the lock (after waking), so it cannot free/return
        // before we finish -- no wake of a freed task.
        spin_lock(&s_ksec_lock);
        ksec_slot_t* slot = ksec_slot_find(resp.seq);
        if (slot && !slot->done) {
            slot->resp = resp;
            slot->done = 1;
            if (slot->waiter) sched_wake(slot->waiter);
        }
        spin_unlock(&s_ksec_lock);
        // Unmatched / stale response (no slot): discarded silently.
    }
}

// ── setuid-on-exec escalation ──────────────────────────────────────────────

void ksec_exec_setuid(cred_t* target, uint32_t setuid_uid, uint32_t inode,
                      uint32_t caller_pid, uint32_t caller_uid,
                      uint32_t caller_gid) {
    int agent = ksec_agent_present();
    // Fast path: no escalation requested, or no agent -> fail closed (no change).
    if (setuid_uid == 0xFFFFFFFFu || !agent) return;

    ksec_request_t req;  __builtin_memset(&req, 0, sizeof(req));
    ksec_response_t resp; __builtin_memset(&resp, 0, sizeof(resp));
    req.seq        = ksec_next_seq();
    req.op         = KSEC_OP_EXEC_SETUID;
    req.caller_pid = caller_pid;
    req.caller_uid = caller_uid;
    req.caller_gid = caller_gid;
    req.inode      = inode;
    req.dev        = 0;

    int rc = ksec_request(&req, &resp);
    if (!ksec_exec_setuid_should_apply(setuid_uid, agent, rc, resp.verdict))
        return;   // denied / failed -> keep inherited euid

    // ksec authorised: it is the authority on the resulting creds.  exec sets
    // both effective and saved set-IDs; the real uid/gid are unchanged.
    target->euid = resp.granted_euid;
    target->suid = resp.granted_euid;
    if (resp.granted_egid) {
        target->egid = resp.granted_egid;
        target->sgid = resp.granted_egid;
    }
}

#ifdef MAKAOS_BOOT_SELFTESTS
// Deterministic test of the fail-closed setuid-on-exec decision gate.
void ksec_exec_setuid_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    // (setuid_uid, agent_present, ksec_rc, verdict) -> expected apply?
    struct { uint32_t su; int agent; int rc; uint8_t v; int want; } c[] = {
        { 0xFFFFFFFFu, 1, 0, KSEC_VERDICT_ALLOW, 0 },  // no setuid bit -> never
        { 0,           0, 0, KSEC_VERDICT_ALLOW, 0 },  // no agent -> fail closed
        { 0,           1, -1, KSEC_VERDICT_ALLOW, 0 }, // round-trip failed -> no
        { 0,           1, 0, KSEC_VERDICT_DENY,  0 },  // explicit deny -> no
        { 0,           1, 0, KSEC_VERDICT_CHALLENGE, 0 }, // challenge != allow -> no
        { 1234,        1, 0, KSEC_VERDICT_ALLOW, 1 },  // the ONLY apply case
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        int got = ksec_exec_setuid_should_apply(c[i].su, c[i].agent, c[i].rc, c[i].v);
        if (got != c[i].want) {
            kprintf_atomic("[ksec_setuid] FAIL i=%u got=%d want=%d\n", i, got, c[i].want);
            fails++;
        }
    }
    kprintf_atomic(fails ? "[ksec_setuid] SELF-TEST FAILED\n"
                  : "[ksec_setuid] SELF-TEST PASSED (setuid-exec escalation fails closed)\n");
}

// Deterministic test of the per-seq slot bookkeeping (the SMP rendezvous fix):
// distinct seqs claim distinct slots, ksec_slot_find matches by seq, seq 0 and
// an unknown seq match nothing, and a freed slot is reusable.  The cross-CPU
// overwrite/UAF is not deterministically reproducible -> this proves the slot
// table logic; code-proof covers the concurrent case (every slot access under
// s_ksec_lock; the requester frees its slot under the lock after the wake).
void ksec_slot_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    spin_lock(&s_ksec_lock);
    for (int i = 0; i < KSEC_MAX_INFLIGHT; i++) {
        s_slots[i].seq = 0; s_slots[i].waiter = (task_t*)0; s_slots[i].done = 0;
    }
    // Claim three distinct seqs -> three distinct free slots.
    ksec_slot_t* a = (ksec_slot_t*)0; ksec_slot_t* b = (ksec_slot_t*)0; ksec_slot_t* c = (ksec_slot_t*)0;
    for (int i = 0; i < KSEC_MAX_INFLIGHT && !a; i++) if (s_slots[i].seq == 0) { s_slots[i].seq = 10; a = &s_slots[i]; }
    for (int i = 0; i < KSEC_MAX_INFLIGHT && !b; i++) if (s_slots[i].seq == 0) { s_slots[i].seq = 20; b = &s_slots[i]; }
    for (int i = 0; i < KSEC_MAX_INFLIGHT && !c; i++) if (s_slots[i].seq == 0) { s_slots[i].seq = 30; c = &s_slots[i]; }
    if (!a || !b || !c || a == b || b == c || a == c) fails++;
    // ksec_slot_find returns the right slot, and nothing for seq 0 / unknown.
    if (ksec_slot_find(10) != a || ksec_slot_find(20) != b || ksec_slot_find(30) != c) fails++;
    if (ksec_slot_find(99) != (ksec_slot_t*)0) fails++;
    if (ksec_slot_find(0)  != (ksec_slot_t*)0) fails++;
    // Free b -> its slot is reusable, find(20) now matches nothing.
    b->seq = 0; b->waiter = (task_t*)0;
    if (ksec_slot_find(20) != (ksec_slot_t*)0) fails++;
    a->seq = 0; c->seq = 0;   // clean the table back to all-free
    spin_unlock(&s_ksec_lock);
    kprintf_atomic(fails ? "[ksec_slot] SELF-TEST FAILED\n"
                  : "[ksec_slot] SELF-TEST PASSED (per-seq slot claim/find/free)\n");
}
#endif /* MAKAOS_BOOT_SELFTESTS */
