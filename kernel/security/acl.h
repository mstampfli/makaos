#pragma once
#include "common.h"
#include "cred.h"

// ── Access Control Lists ──────────────────────────────────────────────────
//
// ACL entries extend the classic POSIX rwx owner/group/other model.
// The first three entries of every ACL ARE the POSIX entries — in order:
//   [0] ACL_TAG_USER  uid=inode.uid   (owner)
//   [1] ACL_TAG_GROUP gid=inode.gid   (owning group)
//   [2] ACL_TAG_OTHER                 (everyone else)
//
// Additional entries follow for extra users/groups.  They are stored in the
// inode's extended attributes under the key "system.acl" as a packed array
// of acl_entry_t.  Files with no extra entries beyond the POSIX three have
// no xattr — zero overhead, pure POSIX behavior.
//
// vfs_check_perm() walks entries in order; first match wins.
//
// ACL permission bits (independent from the mode field, same semantics):
#define ACL_PERM_READ   4u   // r — matches Unix rwx bit ordering (r=4,w=2,x=1)
#define ACL_PERM_WRITE  2u   // w
#define ACL_PERM_EXEC   1u   // x

// ACL entry tag: what the qualifier field means.
#define ACL_TAG_USER    0   // qualifier = uid  (0xFFFFFFFF = any user)
#define ACL_TAG_GROUP   1   // qualifier = gid  (0xFFFFFFFF = any group)
#define ACL_TAG_OTHER   2   // qualifier ignored — matches if nothing else did

#define ACL_WILDCARD    0xFFFFFFFFu   // matches any uid/gid

#define ACL_MAX_ENTRIES 32   // per inode; covers all realistic use cases

typedef struct __attribute__((packed)) {
    uint8_t  tag;        // ACL_TAG_USER / ACL_TAG_GROUP / ACL_TAG_OTHER
    uint8_t  perms;      // ACL_PERM_READ | ACL_PERM_WRITE | ACL_PERM_EXEC
    uint16_t _pad;
    uint32_t qualifier;  // uid or gid (ACL_WILDCARD for other/wildcard)
} acl_entry_t;

// Build the canonical 3-entry POSIX ACL from an inode's mode word.
// mode bits: owner=bits[8:6], group=bits[5:3], other=bits[2:0]
// uid/gid are the inode's owner uid and gid.
static inline void acl_from_mode(acl_entry_t out[3],
                                  uint32_t uid, uint32_t gid, uint16_t mode) {
    // ACL_PERM_* matches Unix r=4,w=2,x=1 so raw mode bits map directly.
    out[0].tag = ACL_TAG_USER;  out[0].qualifier = uid;          out[0].perms = (mode >> 6) & 7; out[0]._pad = 0;
    out[1].tag = ACL_TAG_GROUP; out[1].qualifier = gid;          out[1].perms = (mode >> 3) & 7; out[1]._pad = 0;
    out[2].tag = ACL_TAG_OTHER; out[2].qualifier = ACL_WILDCARD; out[2].perms = (mode)      & 7; out[2]._pad = 0;
}

// Check whether credentials `c` satisfy `need` (ACL_PERM_* bits) given
// the ACL entries `entries[count]`.
// Returns 1 if allowed, 0 if denied.
static inline int acl_check(const acl_entry_t* entries, uint32_t count,
                             const cred_t* c, uint8_t need) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        const acl_entry_t* e = &entries[i];
        int match = 0;
        switch (e->tag) {
        case ACL_TAG_USER:
            match = (e->qualifier == ACL_WILDCARD) ||
                    (e->qualifier == c->euid);
            break;
        case ACL_TAG_GROUP:
            match = (e->qualifier == ACL_WILDCARD) ||
                    cred_in_group(c, e->qualifier);
            break;
        case ACL_TAG_OTHER:
            match = 1;
            break;
        default:
            continue;
        }
        if (match)
            return (e->perms & need) == need;
    }
    return 0;   // no entry matched — deny
}
