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
