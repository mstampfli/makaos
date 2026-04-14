#pragma once
#include "common.h"
#include "kheap.h"

// ── unveil() — Per-Process Filesystem View Restriction ────────────────────
//
// After a process calls sys_unveil(path, perms), sys_open will reject any
// path that is not a prefix-match of an unveil entry, returning ENOENT —
// the path does not exist to this process.
//
// Rules:
//   - Entries accumulate until sys_unveil_lock() is called.
//   - After lock, no new entries can be added.
//   - fork() inherits the table and the locked state.
//   - exec() PRESERVES the table — the new image starts with the same
//     restrictions. The child can only tighten further, never expand.
//   - An empty (not yet populated) table means full visibility.
//   - Once ANY entry is added, paths NOT matching ANY entry return ENOENT.
//
// Path matching: the unveil path must be a prefix of the requested path,
// with a directory separator boundary:
//   unveil("/home/bob", R) allows "/home/bob/x" and "/home/bob"
//   unveil("/home/bob", R) does NOT allow "/home/bobby"
//
// The entry array is heap-allocated and grows on demand — no fixed cap.

#define UNVEIL_READ    (1u << 0)   // O_RDONLY, stat, readdir
#define UNVEIL_WRITE   (1u << 1)   // O_WRONLY, O_RDWR
#define UNVEIL_EXEC    (1u << 2)   // execve / spawn
#define UNVEIL_CREATE  (1u << 3)   // O_CREAT, mkdir, symlink, link

#define UNVEIL_PATH_MAX 256        // matches KPATH_MAX intent for unveil paths

typedef struct {
    char    path[UNVEIL_PATH_MAX];
    uint8_t perms;   // UNVEIL_READ | UNVEIL_WRITE | UNVEIL_EXEC | UNVEIL_CREATE
} unveil_entry_t;

typedef struct {
    unveil_entry_t* entries;  // heap-allocated array
    uint32_t        count;    // number of active entries
    uint32_t        cap;      // allocated capacity
    uint8_t         locked;   // 1 = no more adds allowed
} unveil_table_t;

// Initialise an empty unveil table (full visibility). No heap allocation yet.
static inline void unveil_init(unveil_table_t* t) {
    t->entries = NULL;
    t->count   = 0;
    t->cap     = 0;
    t->locked  = 0;
}

// Free heap storage for an unveil table (called from task_destroy).
static inline void unveil_free(unveil_table_t* t) {
    if (t->entries) { kfree(t->entries); t->entries = NULL; }
    t->count  = 0;
    t->cap    = 0;
    t->locked = 0;
}

// Copy table (used by fork and spawn). Returns 0 on success, -1 on OOM.
static inline int unveil_copy(unveil_table_t* dst, const unveil_table_t* src) {
    dst->count  = 0;
    dst->cap    = 0;
    dst->locked = src->locked;
    dst->entries = NULL;
    if (src->count == 0) return 0;
    dst->entries = (unveil_entry_t*)kmalloc(src->count * sizeof(unveil_entry_t));
    if (!dst->entries) return -1;
    dst->cap   = src->count;
    dst->count = src->count;
    for (uint32_t i = 0; i < src->count; i++)
        dst->entries[i] = src->entries[i];
    return 0;
}

// Lock the table. After this, unveil_add returns -EPERM.
static inline void unveil_lock(unveil_table_t* t) {
    t->locked = 1;
}

// Add an entry. Returns 0 on success, -errno on error.
// path must be NUL-terminated and absolute (starts with '/').
// Merges perms if path already exists. Grows array on demand.
int unveil_add(unveil_table_t* t, const char* path, uint8_t perms);

// Check whether `path` is allowed with `need` (UNVEIL_* bits).
// Returns 1 if allowed, 0 if denied.
// An empty table (count == 0) always returns 1 (full visibility).
int unveil_check(const unveil_table_t* t, const char* path, uint8_t need);
