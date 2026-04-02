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

// List directory at `path`. Fills `entries` (up to `max`).
// Skips "." and ".." and deleted entries (inode==0).
// Returns count of entries written, or -1 on error.
int ext2_readdir(const char* path, ext2_entry_t* entries, int max);

// Create or overwrite a file at `path` with `size` bytes from `data`.
// Parent directory must already exist.
// Returns 1 on success, 0 on failure.
int ext2_write_file(const char* path, const uint8_t* data, uint32_t size);

// Create a directory at `path` (parent must exist).
// Returns 1 on success, 0 on failure.
int ext2_mkdir(const char* path);
