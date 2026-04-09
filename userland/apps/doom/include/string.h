#pragma once
// string.h shim — redirects to our libc
#include "../../../libc/libc.h"

// Additional string functions
static inline char* strpbrk(const char* s, const char* accept) {
    while (*s) {
        const char* a = accept;
        while (*a) { if (*s == *a) return (char*)s; a++; }
        s++;
    }
    return (char*)0;
}

static inline size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (s[n]) {
        const char* a = accept;
        int found = 0;
        while (*a) { if (s[n] == *a) { found = 1; break; } a++; }
        if (!found) break;
        n++;
    }
    return n;
}

static inline size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (s[n]) {
        const char* r = reject;
        while (*r) { if (s[n] == *r) return n; r++; }
        n++;
    }
    return n;
}

static inline char* strtok(char* s, const char* delim) {
    static char* saved = (char*)0;
    if (s) saved = s;
    if (!saved || !*saved) return (char*)0;
    // Skip leading delimiters.
    while (*saved && strchr(delim, *saved)) saved++;
    if (!*saved) return (char*)0;
    char* start = saved;
    while (*saved && !strchr(delim, *saved)) saved++;
    if (*saved) *saved++ = '\0';
    return start;
}

static inline void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char uc = (unsigned char)c;
    for (size_t i = 0; i < n; i++) if (p[i] == uc) return (void*)(p + i);
    return (void*)0;
}

static inline char* strerror(int err) {
    (void)err; return "error";
}
