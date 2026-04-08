#pragma once
// stdio.h shim — redirects to our stdio layer.
// Define _DOOM_RENAME_OVERRIDE so libc.h does not install the 4-arg rename macro.
#define _DOOM_RENAME_OVERRIDE

#include "../../stdio.h"
#include "../../libc.h"

// POSIX 2-arg rename using direct syscall (SYS_RENAME=17).
#undef rename
static inline int rename(const char* old_path, const char* new_path) {
    unsigned long long ol = (unsigned long long)__builtin_strlen(old_path);
    unsigned long long nl = (unsigned long long)__builtin_strlen(new_path);
    unsigned long long ret;
    register unsigned long long r10 __asm__("r10") = (unsigned long long)new_path;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(17ULL), "D"((unsigned long long)old_path), "S"(ol), "d"(nl), "r"(r10)
        : "rcx", "r11", "memory"
    );
    long r = (long)ret;
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return 0;
}

static inline int remove(const char* path) {
    unsigned long long n = (unsigned long long)__builtin_strlen(path);
    unsigned long long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(16ULL), "D"((unsigned long long)path), "S"(n), "d"(0ULL)
        : "rcx", "r11", "memory"
    );
    long r = (long)ret;
    if (r < 0 && r > -4096) { errno = (int)-r; return -1; }
    return 0;
}

// scanf is not used by Doom core — stub only.
static inline int scanf(const char* fmt, ...) { (void)fmt; return 0; }
static inline int fscanf(FILE* f, const char* fmt, ...) { (void)f; (void)fmt; return 0; }
