#pragma once
#include "common.h"
#include "cred.h"
#include "acl.h"

// ── Unified Permission Check ──────────────────────────────────────────────
//
// vfs_check_perm() is the single entry point for all access control decisions
// at open/exec time.  It runs in two phases:
//
//   Phase 1 — fast path (no allocation):
//     Build the 3-entry POSIX ACL from uid/gid/mode and call acl_check().
//     This handles 99%+ of real requests with no heap access.
//
//   Phase 2 — slow path (only if xattr ACL exists):
//     Load the extended ACL entries from the inode's "system.acl" xattr
//     and walk them.  First match wins.
//
// `need` is a bitmask of ACL_PERM_READ | ACL_PERM_WRITE | ACL_PERM_EXEC.
// Returns 0 if allowed, -EACCES if denied.

#include "errno.h"

// Inode permission snapshot — populated by the caller from whatever
// filesystem structure is in use (ext2_inode_t, proc virtual, etc.).
typedef struct {
    uint32_t uid;       // inode owner uid
    uint32_t gid;       // inode owner gid
    uint16_t mode;      // standard POSIX mode bits (rwxrwxrwx + setuid/setgid/sticky)
    uint32_t inode_nr;  // inode number (used for xattr lookup)
    uint32_t dev;       // device number (used for xattr lookup + nosuid check)
    uint8_t  nosuid;    // 1 = mounted nosuid (setuid bit ignored on exec)
} inode_perm_t;

// Defined in perm.c.
// Returns 0 (allowed) or -EACCES (denied).
int vfs_check_perm(const inode_perm_t* ip, const cred_t* c, uint8_t need);

// Check execute permission AND whether the setuid bit should be honoured.
// If the setuid bit is set on the inode and the mount is not nosuid:
//   - Sets *out_setuid_uid to the inode's uid (caller must ask ksec).
//   - Sets *out_setuid_uid to 0xFFFFFFFF if no setuid escalation applies.
// Returns 0 (allowed) or -EACCES (denied).
int vfs_check_exec(const inode_perm_t* ip, const cred_t* c,
                   uint32_t* out_setuid_uid);

// Mount flags stored per-mount (minimal — just what the security layer needs).
#define MOUNT_FLAG_NOSUID  (1u << 0)
#define MOUNT_FLAG_NOEXEC  (1u << 1)
#define MOUNT_FLAG_RDONLY  (1u << 2)
