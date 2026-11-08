// ── string.c — <string.h> extern wrappers for sysroot consumers ─────
//
// libc.c holds the bulk of the string routines (memcpy/memmove/memset/
// memcmp/strlen/strcmp/strncmp/strcpy/strcat/...).  memchr lived in the
// old junk-drawer syscalls.c; it belongs here semantically.

#include <string.h>

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char target = (unsigned char)c;
    while (n--) {
        if (*p == target) return (void*)p;
        p++;
    }
    return (void*)0;
}

// Copy up to n bytes; stop AFTER copying the first byte equal to c, returning a
// pointer to the byte in dest just past it, or NULL if c was not among them.
void* memccpy(void* dest, const void* src, int c, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    unsigned char target = (unsigned char)c;
    while (n--) {
        if ((*d++ = *s++) == target) return (void*)d;
    }
    return (void*)0;
}
