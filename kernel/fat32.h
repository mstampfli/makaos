#pragma once
#include "common.h"
#include "vfs.h"

// ── FAT32 driver ──────────────────────────────────────────────────────────
//
// Mounts a FAT32 volume that starts at a given LBA on the primary ATA disk.
// Provides open/read/close for regular files, accessible via VFS.
//
// Constraints (kernel context):
//   - All disk I/O is synchronous from the caller's perspective: the caller
//     yields (sched_yield) while the ATA state machine runs, so disk reads
//     must be called from process context ONLY — never from an IRQ handler.
//   - No dynamic memory for directory traversal: uses a small static sector
//     buffer (one sector = 512 bytes) shared across all FAT32 calls.
//     Callers must not be re-entrant (single CPU, cooperative scheduler — fine).
//   - Only supports reading; no write support yet.
//   - Path format: flat — "FILENAME.EXT" (no subdirectories for now).
//     8.3 names, uppercase.

// Initialise the FAT32 layer.  Reads the BPB from `part_lba`.
// Returns 1 on success, 0 on failure.
// Must be called from process context after ATA is initialised.
uint8_t fat32_init(uint32_t part_lba);

// Open a file by 8.3 name (e.g. "HELLO   BIN", exactly 11 chars, uppercase,
// space-padded).  Returns a heap-allocated vfs_file_t* or NULL if not found.
// Close with vfs_close() when done — this frees the backing fat32_fd_t.
vfs_file_t* fat32_open(const char* name83);

// Mount-time LBA of the FAT32 partition (set by fat32_init).
extern uint32_t g_fat32_part_lba;

// ── Directory entry (user-visible) ────────────────────────────────────────
typedef struct {
    char     name[13];   // human-readable "filename.ext\0"
    char     name83[11]; // raw 8.3 name (no null terminator)
    uint32_t size;       // file size in bytes (0 for directories)
    uint8_t  is_dir;     // 1 if directory, 0 if file
} fat32_entry_t;

// Enumerate the root directory.  Fills `entries` (up to max_entries).
// Skips deleted (0xE5) and LFN entries.  Stops at first 0x00 entry.
// Returns the number of entries written.
int fat32_readdir(fat32_entry_t* entries, int max_entries);

// Write (create or overwrite) a file in the root directory.
// `name83` must be exactly 11 bytes, uppercase, space-padded (8.3 format).
// Returns 1 on success, 0 on failure.
int fat32_write_file(const char* name83, const uint8_t* data, uint32_t size);
