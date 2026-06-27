#pragma once
#include "common.h"
#include "vfs.h"

// ── ext2 filesystem driver ────────────────────────────────────────────────
//
// Mounts an ext2 volume that starts at a given LBA on the primary ATA disk.
// Supports: open/read files, readdir, write_file (create/overwrite), mkdir.
//
// Constraints:
//   - Assumes 1024-byte blocks (mkfs.ext2 default for small volumes).
//   - All disk I/O is synchronous polling (ata_poll_read28/write28).
//   - No re-entrancy (single CPU, cooperative scheduler).

// ── On-disk structures (packed, little-endian) ────────────────────────────

// Superblock: sits at byte offset 1024 from the partition start.
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;   // 0 for 4KB blocks, 1 for 1KB blocks
    uint32_t s_log_block_size;     // block_size = 1024 << s_log_block_size
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              // 0xEF53
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // EXT2_DYNAMIC_REV fields (rev_level >= 1)
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // Padding to fill the 1024-byte superblock
    uint8_t  _pad[820];
} ext2_superblock_t;

// Block group descriptor: 32 bytes each. Array starts at block 1 (1KB blocks).
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;      // block number of block usage bitmap
    uint32_t bg_inode_bitmap;      // block number of inode usage bitmap
    uint32_t bg_inode_table;       // block number of inode table start
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_bgd_t;

// Inode: 128 bytes (rev 0/1 format).
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;    // in 512-byte units
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15]; // [0..11] direct, [12] single-indirect, [13] double, [14] triple
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

// Directory entry (variable length). Minimum 8 bytes + name.
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;  // 1=regular, 2=dir, 0=unknown
    char     name[255];  // NOT null-terminated in actual record
} ext2_dirent_t;

// ── Constants ─────────────────────────────────────────────────────────────

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_FT_UNKNOWN     0
#define EXT2_FT_REG_FILE    1
#define EXT2_FT_DIR         2
#define EXT2_S_IFREG        0x8000
#define EXT2_S_IFDIR        0x4000

// ── User-visible directory entry (returned by ext2_readdir) ──────────────
typedef struct {
    char     name[256];
    uint32_t inode_num;
    uint32_t size;
    uint8_t  is_dir;
} ext2_entry_t;

// ── Public API ────────────────────────────────────────────────────────────

// Initialise ext2, reading superblock from `part_lba`.
// Returns 1 on success, 0 on failure.
uint8_t ext2_init(uint32_t part_lba);

// Open a file by absolute path (e.g. "/bin/sh").
// Returns heap-allocated vfs_file_t* (close with vfs_close), or NULL.
vfs_file_t* ext2_open(const char* path);

// Open a file by its already-resolved inode number.  Skips the path walk
// entirely — use this when the caller already holds the inode from a prior
// ext2_lookup_path() call.  `path` is optional (stored in f->path for
// fstat/ftruncate); pass NULL if unavailable.
// Returns heap-allocated vfs_file_t* (close with vfs_close), or NULL.
vfs_file_t* ext2_open_ino(uint32_t ino, const char* path);

// List directory at `path`. Fills `entries` (up to `max`).
// Skips "." and ".." and deleted entries (inode==0).
// Returns count of entries written, or -1 on error.
int ext2_readdir(const char* path, ext2_entry_t* entries, int max);

// Create or overwrite a file at `path` with `size` bytes from `data`.
// Parent directory must already exist.
// Returns 1 on success, 0 on failure.
// `cred` (a cred_t*) gates NEW-file creation on parent write+exec, checked on
// the parent inode THIS call resolves (closing the path-TOCTOU); NULL skips it
// (internal overwrite/truncate of an existing file, which never creates).
int ext2_write_file(const char* path, const uint8_t* data, uint32_t size,
                    const void* cred);

// Create an empty file at `path`.  Fails if the file already exists.
// Returns 1 on success, 0 on failure (including EEXIST).  `cred` (a cred_t*) is
// checked for write+exec on the parent dir the create resolves into.
int ext2_create(const char* path, const void* cred);

// Truncate the file at `path` to zero bytes.
// Returns 1 on success, 0 on failure.
int ext2_truncate(const char* path);

// Create a directory at `path` (parent must exist).  `cred` (a cred_t*) is
// checked for write+exec on the resolved parent dir.
int ext2_mkdir(const char* path, const void* cred);

// Remove a regular file at `path`.  `cred` (a cred_t*) is checked for write+exec
// on the resolved parent dir.
int ext2_unlink(const char* path, const void* cred);

// Rename/move `src` to `dst` (both must be on the same volume).  `cred` (a
// cred_t*) is checked for write+exec on BOTH resolved parent dirs.
int ext2_rename(const char* src, const char* dst, const void* cred);

// Truncate or extend file at `path` to exactly `length` bytes.
// Returns 1 on success, 0 on failure.
int ext2_truncate_to(const char* path, uint64_t length);

// Permission-checked path resolution.
// Permission-checked path walk — for syscall handlers acting on behalf of a user.
// Checks execute (search) on every directory component against `cred`.
// Returns inode number on success, 0 on failure (*err_out set to -ENOENT/-EACCES).
uint32_t ext2_lookup_path(const char* path, const void* cred, int* err_out);

// Unchecked path walk — for kernel-internal callers that already hold an open
// fd (e.g. fstat) or run in a kthread with no user context (no cred to check).
// Never use this from a syscall handler on a fresh path.
static inline uint32_t ext2_lookup_path_raw(const char* path) {
    return ext2_lookup_path(path, (void*)0, (void*)0);
}

// Read inode `ino` into `*out`.  Returns 1 on success, 0 on failure.
uint8_t ext2_read_inode(uint32_t ino, ext2_inode_t* out);
