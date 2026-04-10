#include "unveil.h"
#include "errno.h"

// ── String helpers (no libc in kernel) ───────────────────────────────────

static uint32_t s_strlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int s_strncmp(const char* a, const char* b, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void s_strncpy(char* dst, const char* src, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

// ── unveil_add ────────────────────────────────────────────────────────────

int unveil_add(unveil_table_t* t, const char* path, uint8_t perms) {
    uint32_t i;
    uint32_t plen;

    if (!t || !path) return -EINVAL;
    if (t->locked)   return -EPERM;
    if (path[0] != '/') return -EINVAL;   // must be absolute

    plen = s_strlen(path);
    if (plen == 0 || plen >= UNVEIL_PATH_MAX) return -ENAMETOOLONG;

    // Update an existing entry for the same path (merging perms).
    for (i = 0; i < t->count; i++) {
        if (t->entries[i].active &&
            s_strncmp(t->entries[i].path, path, UNVEIL_PATH_MAX) == 0) {
            t->entries[i].perms |= perms;
            return 0;
        }
    }

    if (t->count >= UNVEIL_MAX) return -ENOSPC;

    t->entries[t->count].active = 1;
    t->entries[t->count].perms  = perms;
    s_strncpy(t->entries[t->count].path, path, UNVEIL_PATH_MAX);
    t->count++;
    return 0;
}

// ── unveil_check ──────────────────────────────────────────────────────────

int unveil_check(const unveil_table_t* t, const char* path, uint8_t need) {
    uint32_t i;
    uint32_t plen;
    uint32_t elen;

    if (!t || !path) return 0;

    // Empty table = full visibility (process has not called unveil yet).
    if (t->count == 0) return 1;

    plen = s_strlen(path);

    for (i = 0; i < t->count; i++) {
        if (!t->entries[i].active) continue;

        elen = s_strlen(t->entries[i].path);

        // The unveil path must be a prefix of the requested path.
        if (plen < elen) continue;
        if (s_strncmp(path, t->entries[i].path, elen) != 0) continue;

        // Boundary check: after the prefix the requested path must end or
        // have a '/' — prevents "/home/bob" from matching "/home/bobby".
        if (plen > elen && path[elen] != '/') continue;

        // Prefix matched — check permissions.
        return (t->entries[i].perms & need) == need;
    }

    return 0;   // no prefix matched — ENOENT to caller
}
