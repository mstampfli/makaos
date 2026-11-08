#pragma once
#include "common.h"

// ── Process Credentials ───────────────────────────────────────────────────
//
// Full POSIX three-uid / three-gid model plus supplemental groups.
// Embedded directly in task_t (no separate allocation — credentials are
// per-task, not shared across threads; fork copies them, exec may change them).
//
// Setuid/setgid transitions that go OUTSIDE the three-slot POSIX rules
// (i.e. a genuine privilege escalation) require ksec authorisation.
// In-slot transitions (e.g. seteuid to ruid or suid) are handled directly
// by the kernel without consulting ksec.

#define CRED_NGROUPS_MAX  32    // maximum supplemental groups per process

typedef struct {
    uint32_t ruid;   // real user ID
    uint32_t euid;   // effective user ID  (used for all permission checks)
    uint32_t suid;   // saved set-user-ID  (POSIX mandated)

    uint32_t rgid;   // real group ID
    uint32_t egid;   // effective group ID
    uint32_t sgid;   // saved set-group-ID

    uint32_t supplemental[CRED_NGROUPS_MAX];
    uint8_t  ngroups;   // number of valid entries in supplemental[]
} cred_t;

// Initialise credentials for a new kernel thread (uid=0, all zeros).
static inline void cred_init_root(cred_t* c) {
    c->ruid = c->euid = c->suid = 0;
    c->rgid = c->egid = c->sgid = 0;
    c->ngroups = 0;
}

// Initialise credentials for a freshly-forked child (copy from parent).
static inline void cred_copy(cred_t* dst, const cred_t* src) {
    uint32_t i;
    dst->ruid = src->ruid; dst->euid = src->euid; dst->suid = src->suid;
    dst->rgid = src->rgid; dst->egid = src->egid; dst->sgid = src->sgid;
    dst->ngroups = src->ngroups;
    for (i = 0; i < src->ngroups && i < CRED_NGROUPS_MAX; i++)
        dst->supplemental[i] = src->supplemental[i];
}

// Returns 1 if gid matches egid or any supplemental group.
static inline int cred_in_group(const cred_t* c, uint32_t gid) {
    uint8_t i;
    if (c->egid == gid) return 1;
    for (i = 0; i < c->ngroups; i++)
        if (c->supplemental[i] == gid) return 1;
    return 0;
}

// Returns 1 if the process is running as effective root.
static inline int cred_is_root(const cred_t* c) {
    return c->euid == 0;
}

// True iff `uid` (resp. `gid`) is one of the process's real / effective / saved
// IDs -- the POSIX seteuid()/setegid() eligibility test for a non-root process,
// also the uid/gid membership half of spawn_cred_allowed.  One source of truth
// so the 3-slot set can't drift (a dropped slot is a privilege bug).  NOTE:
// setuid()/setgid() deliberately use a DIFFERENT 2-slot {real,saved} set, so
// they do NOT use these.
static inline int cred_uid_is_one_of(const cred_t* c, uint32_t uid) {
    return uid == c->ruid || uid == c->euid || uid == c->suid;
}
static inline int cred_gid_is_one_of(const cred_t* c, uint32_t gid) {
    return gid == c->rgid || gid == c->egid || gid == c->sgid;
}

// POSIX setuid() rules (no escalation — in-slot only).
// Returns 0 on success, -1 if the transition is outside the three slots
// (caller must then ask ksec).
static inline int cred_setuid(cred_t* c, uint32_t uid) {
    if (c->euid == 0) {
        // root: set all three
        c->ruid = c->suid = c->euid = uid;
        return 0;
    }
    if (uid == c->ruid || uid == c->suid) {
        c->euid = uid;
        return 0;
    }
    return -1;   // escalation — needs ksec
}

// POSIX seteuid() rules.
static inline int cred_seteuid(cred_t* c, uint32_t euid) {
    if (c->euid == 0) { c->euid = euid; return 0; }
    if (cred_uid_is_one_of(c, euid)) {
        c->euid = euid;
        return 0;
    }
    return -1;   // escalation — needs ksec
}

// POSIX setgid() rules (mirrored).
static inline int cred_setgid(cred_t* c, uint32_t gid) {
    if (c->euid == 0) {
        c->rgid = c->sgid = c->egid = gid;
        return 0;
    }
    if (gid == c->rgid || gid == c->sgid) {
        c->egid = gid;
        return 0;
    }
    return -1;
}

static inline int cred_setegid(cred_t* c, uint32_t egid) {
    if (c->euid == 0) { c->egid = egid; return 0; }
    if (cred_gid_is_one_of(c, egid)) {
        c->egid = egid;
        return 0;
    }
    return -1;
}
