#pragma once
#include "common.h"   // uint*_t, NULL

// ── Freestanding C-string helpers ───────────────────────────────────────────
//
// Small, exact-semantics NUL-terminated-string utilities shared across the
// kernel so the truncation / termination rules live in ONE place instead of
// being hand-rolled (and drifting) per file.  All are pure inlines.

// Unbounded, case-sensitive equality: returns 1 iff `a` and `b` are equal and
// terminate together.  Args must be non-NULL.
static inline int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

// Length of a NUL-terminated string (kernel strings are bounded well under 4G).
static inline uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

// Truncating copy into a `size`-byte buffer: copies at most size-1 bytes of
// `src` and ALWAYS writes the NUL terminator (when size > 0).  Does not
// zero-pad the tail.  A size of 0 is a no-op; a NULL src yields an empty
// string.  This is the s_strncpy / strncpy_k contract, with the size==0 /
// NULL-src guards added so it is safe by construction.
static inline void str_lcpy(char* dst, const char* src, uint32_t size) {
    if (size == 0) return;
    uint32_t i = 0;
    if (src) for (; i < size - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Split `path` at its last '/': writes the parent prefix (everything before the
// last '/') as a NUL-terminated string into `parent_out` and returns a pointer
// to the basename (the suffix after the last '/').  Bounded by construction:
// returns NULL if the parent does not fit in `cap` bytes -- an unbounded copy
// here overran a fixed caller buffer (reachable via a ~511-byte path), smashing
// the kernel stack, so every caller MUST treat NULL as an error.  With no '/'
// (or only a leading one) the parent is "/".  The single dirname primitive for
// the whole kernel (ext2 metadata ops, virtfs parent-perm refinement, syscall
// dirname splits) so the bound cannot drift per file.
static inline const char* path_split(const char* path, char* parent_out,
                                     uint32_t cap) {
    if (cap < 2) return NULL;            // need room for at least "/" + NUL
    uint32_t len = str_len(path);
    // Find last '/'.
    int last_slash = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    if (last_slash <= 0) {
        // Parent is "/".
        parent_out[0] = '/';
        parent_out[1] = '\0';
        // Basename starts after '/' at position 0, or at 0 if no slash.
        return (last_slash == 0) ? path + 1 : path;
    }

    // Bound the copy: we write parent_out[0..last_slash-1] + a NUL at
    // [last_slash], so we need last_slash < cap.  Reject (NULL) otherwise;
    // every caller already treats a NULL return as an error.
    if ((uint32_t)last_slash >= cap) return NULL;
    for (int i = 0; i < last_slash; i++) parent_out[i] = path[i];
    parent_out[last_slash] = '\0';
    return path + last_slash + 1;
}
