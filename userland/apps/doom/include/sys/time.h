#pragma once
// sys/time.h shim
#include "../../../../libc/libc.h"

typedef struct { long tv_sec; long tv_usec; } timeval;

static inline int gettimeofday(timeval* tv, void* tz) {
    (void)tz;
    // Use SYS_GETTOD
    typedef struct { long long tv_sec; long long tv_usec; } k_tv_t;
    k_tv_t ktv = {0, 0};
    register unsigned long long rax __asm__("rax") = 32; // SYS_GETTOD
    register unsigned long long rdi __asm__("rdi") = (unsigned long long)&ktv;
    register unsigned long long rsi __asm__("rsi") = 0;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi) : "rcx", "r11", "memory");
    tv->tv_sec  = (long)ktv.tv_sec;
    tv->tv_usec = (long)ktv.tv_usec;
    return 0;
}
