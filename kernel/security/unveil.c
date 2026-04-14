#include "unveil.h"
#include "kheap.h"
#include "errno.h"

// ── String helpers (no libc in kernel) ───────────────────────────────────

static uint32_t s_strlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

static int s_strncmp(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void s_strncpy(char* dst, const char* src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// ── unveil_add ────────────────────────────────────────────────────────────

int unveil_add(unveil_table_t* t, const char* path, uint8_t perms) {
    if (!t || !path)    return -EINVAL;
    if (t->locked)      return -EPERM;
    if (path[0] != '/') return -EINVAL;

    uint32_t plen = s_strlen(path);
    if (plen == 0 || plen >= UNVEIL_PATH_MAX) return -ENAMETOOLONG;

    // Merge perms if path already exists — O(n) scan, acceptable since
    // unveil tables are small (tens of entries at most, set at startup).
    for (uint32_t i = 0; i < t->count; i++) {
        if (s_strncmp(t->entries[i].path, path, UNVEIL_PATH_MAX) == 0) {
            t->entries[i].perms |= perms;
            return 0;
        }
    }

    // Grow backing array if needed. Double capacity each time to amortise.
    if (t->count >= t->cap) {
        uint32_t new_cap = t->cap ? t->cap * 2 : 4;
        unveil_entry_t* nb = (unveil_entry_t*)kmalloc(new_cap * sizeof(unveil_entry_t));
        if (!nb) return -ENOMEM;
        for (uint32_t i = 0; i < t->count; i++) nb[i] = t->entries[i];
        kfree(t->entries);
        t->entries = nb;
        t->cap     = new_cap;
    }

    t->entries[t->count].perms = perms;
    s_strncpy(t->entries[t->count].path, path, UNVEIL_PATH_MAX);
    t->count++;
    return 0;
}

// ── unveil_check ──────────────────────────────────────────────────────────

int unveil_check(const unveil_table_t* t, const char* path, uint8_t need) {
    if (!t || !path) return 0;
    if (t->count == 0) return 1;   // empty table = full visibility

    uint32_t plen = s_strlen(path);

    for (uint32_t i = 0; i < t->count; i++) {
        uint32_t elen = s_strlen(t->entries[i].path);
        if (plen < elen) continue;
        if (s_strncmp(path, t->entries[i].path, elen) != 0) continue;
        // Boundary check: prevents "/home/bob" matching "/home/bobby".
        if (plen > elen && path[elen] != '/') continue;
        return (t->entries[i].perms & need) == need;
    }

    return 0;
}
