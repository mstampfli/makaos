#pragma once
#include "common.h"

// ── unveil() — Per-Process Filesystem View Restriction ────────────────────
//
// After a process calls sys_unveil(path, perms), sys_open will reject any
// path that is not a prefix-match of an unveil entry, returning ENOENT —
// the path does not exist to this process.
//
// Rules:
//   - Entries accumulate until sys_unveil_lock() is called (or exec).
//   - After lock, no new entries can be added.
//   - fork() inherits the table and the locked state.
//   - exec() resets to empty (full visibility) so the new image can set
//     its own unveil. The parent can unveil_lock before exec to prevent
//     the child from expanding.
//   - An empty (not yet populated) table means full visibility.
//   - Once ANY entry is added, paths NOT matching ANY entry return ENOENT.
//
// Path matching: the unveil path must be a prefix of the requested path,
// with a directory separator boundary:
//   unveil("/home/bob", R) allows "/home/bob/x" and "/home/bob"
//   unveil("/home/bob", R) does NOT allow "/home/bobby"

#define UNVEIL_READ    (1u << 0)   // O_RDONLY, stat, readdir
#define UNVEIL_WRITE   (1u << 1)   // O_WRONLY, O_RDWR
#define UNVEIL_EXEC    (1u << 2)   // execve / spawn
#define UNVEIL_CREATE  (1u << 3)   // O_CREAT, mkdir, symlink, link
#define UNVEIL_MAX     16          // entries per process

#define UNVEIL_PATH_MAX 128

typedef struct {
    char    path[UNVEIL_PATH_MAX];
    uint8_t perms;   // UNVEIL_READ | UNVEIL_WRITE | UNVEIL_EXEC | UNVEIL_CREATE
    uint8_t active;  // 1 = entry is valid
} unveil_entry_t;

typedef struct {
    unveil_entry_t entries[UNVEIL_MAX];
    uint8_t        count;
    uint8_t        locked;   // 1 = no more adds allowed
} unveil_table_t;

// Initialise an empty unveil table (full visibility).
static inline void unveil_init(unveil_table_t* t) {
    uint32_t i;
    t->count  = 0;
    t->locked = 0;
    for (i = 0; i < UNVEIL_MAX; i++) {
        t->entries[i].active = 0;
        t->entries[i].path[0] = '\0';
        t->entries[i].perms = 0;
    }
}

// Copy table (used by fork).
static inline void unveil_copy(unveil_table_t* dst, const unveil_table_t* src) {
    uint32_t i;
    dst->count  = src->count;
    dst->locked = src->locked;
    for (i = 0; i < UNVEIL_MAX; i++)
        dst->entries[i] = src->entries[i];
}

// Reset table (used by exec in the child — new image gets full visibility).
static inline void unveil_reset(unveil_table_t* t) {
    unveil_init(t);
}

// Add an entry. Returns 0 on success, -1 if locked or table full.
// path must be NUL-terminated and absolute (starts with '/').
int unveil_add(unveil_table_t* t, const char* path, uint8_t perms);

// Lock the table. After this, unveil_add returns -1.
static inline void unveil_lock(unveil_table_t* t) {
    t->locked = 1;
}

// Check whether `path` is allowed with `need` (UNVEIL_* bits).
// Returns 1 if allowed, 0 if denied.
// An empty table (count == 0) always returns 1 (full visibility).
int unveil_check(const unveil_table_t* t, const char* path, uint8_t need);
