#include "perm.h"
#include "acl.h"
#include "cred.h"
#include "errno.h"

// ── vfs_check_perm ────────────────────────────────────────────────────────
//
// Phase 1: fast POSIX check (3-entry ACL built from mode bits).
// Phase 2: extended ACL from xattr (not yet implemented — hook is here).
//
// `need` is ACL_PERM_READ | ACL_PERM_WRITE | ACL_PERM_EXEC (any combination).
// Returns 0 if allowed, -EACCES if denied.

int vfs_check_perm(const inode_perm_t* ip, const cred_t* c, uint8_t need) {
    acl_entry_t posix[3];

    if (!ip || !c) return -EINVAL;

    // Root bypasses all DAC checks (matches Linux/BSD semantics for
    // read/write; execute still requires at least one x bit for root).
    if (cred_is_root(c)) {
        if (need & ACL_PERM_EXEC) {
            // Root can only exec if at least one x bit is set anywhere.
            uint8_t any_x = ((ip->mode >> 6) | (ip->mode >> 3) | ip->mode) & 1;
            if (!any_x) return -EACCES;
        }
        return 0;
    }

    // Phase 1: POSIX 3-entry check.
    acl_from_mode(posix, ip->uid, ip->gid, ip->mode);
    if (acl_check(posix, 3, c, need))
        return 0;

    // Phase 2: extended ACL (xattr "system.acl").
    // Hook: if the filesystem exposes xattr_get(inode_nr, "system.acl", ...),
    // load extra entries here and call acl_check again.
    // Currently returns denied — extended ACL entries are not yet stored.
    // When ext2 xattr support is added, this path fills in automatically.

    return -EACCES;
}

// ── vfs_check_exec ────────────────────────────────────────────────────────
//
// Combined execute permission + setuid detection.
// Populates *out_setuid_uid with the inode uid if escalation is needed,
// or 0xFFFFFFFF if no setuid escalation applies.
// Returns 0 (allowed to exec) or -EACCES (denied).

#define S_ISUID 04000   // setuid bit in mode
#define S_ISGID 02000   // setgid bit in mode

int vfs_check_exec(const inode_perm_t* ip, const cred_t* c,
                   uint32_t* out_setuid_uid) {
    int r;

    if (!ip || !c || !out_setuid_uid) return -EINVAL;

    *out_setuid_uid = 0xFFFFFFFFu;   // default: no escalation

    // Check execute permission first.
    r = vfs_check_perm(ip, c, ACL_PERM_EXEC);
    if (r != 0) return r;

    // If nosuid mount: ignore the setuid bit entirely.
    if (ip->nosuid) return 0;

    // If setuid bit is set and inode uid != euid: signal that ksec is needed.
    if ((ip->mode & S_ISUID) && ip->uid != c->euid) {
        *out_setuid_uid = ip->uid;
    }

    // setgid with no group execute is a mandatory-lock flag — ignore here.
    // setgid with group execute: similar handling would go here if needed.

    return 0;
}
