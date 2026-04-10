#pragma once
#include "common.h"
#include "cred.h"
#include "perm.h"

// ── Virtual Filesystem Layer ──────────────────────────────────────────────
//
// /proc and /dev are synthetic — no ext2 inodes back them.  This module
// provides lookup + permission checking for them, mirroring the ext2 side:
//
//   ext2:   ext2_lookup_path(path, cred, need, &err) → inode or 0
//   virtfs: virtfs_lookup(path, cred, need, &info)           → 0 or -errno
//
// On top of both sits a single global entry point:
//
//   fs_lookup(path, cred, need, &info)  → 0 or -errno
//
// Every syscall handler calls fs_lookup — it routes to virtfs or ext2
// internally.  No more if/else chains in syscall.c.

// ── Result type ───────────────────────────────────────────────────────────
// Filled by virtfs_lookup and fs_lookup on success.

#define FS_TYPE_FILE   0   // regular file
#define FS_TYPE_DIR    1   // directory
#define FS_TYPE_CHAR   2   // character device (virtfs /dev nodes)

typedef struct {
    int      type;       // FS_TYPE_*
    int      is_virtual; // 1 = came from virtfs, 0 = came from ext2
    // ext2 fields (valid when is_virtual == 0)
    uint32_t inode_nr;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;       // full inode mode word (type bits + rwx bits)
    uint64_t size;
} fs_node_t;

// ── virtfs API ────────────────────────────────────────────────────────────

// Returns 1 if path is inside a registered virtual mount, 0 otherwise.
int virtfs_is_virtual(const char* path);

// Lookup + permission check for a virtual path.
// `need` is ACL_PERM_READ | ACL_PERM_WRITE | ACL_PERM_EXEC (or 0 for F_OK).
// Returns 0 on success (fills *out), -ENOENT if not virtual, -EACCES if denied.
int virtfs_lookup(const char* path, const cred_t* cred, uint8_t need,
                  fs_node_t* out);

// ── Global entry point ────────────────────────────────────────────────────

// Routes to virtfs_lookup or ext2_lookup_path based on path prefix.
// Returns 0 on success (fills *out), negative errno on failure.
// Every syscall handler should call this instead of the backends directly.
int fs_lookup(const char* path, const cred_t* cred, uint8_t need,
              fs_node_t* out);
