#pragma once

#ifdef NDEBUG
#  define assert(cond)  ((void)0)
#else
// Write a diagnostic to stderr and exit(1) if cond is false.
// Avoids depending on printf to keep the header self-contained.
static inline void __assert_fail(const char* msg) {
    // Direct write syscall: SYS_WRITE=0, fd=2 (stderr)
    __asm__ volatile(
        "syscall"
        :
        : "a"(0ULL), "D"(2ULL),
          "S"(msg),
          "d"(__builtin_strlen(msg))
        : "rcx", "r11", "memory"
    );
    // SYS_EXIT=1
    __asm__ volatile("syscall" : : "a"(1ULL), "D"(1ULL) : "rcx", "r11");
    __builtin_unreachable();
}

#  define assert(cond) \
    do { \
        if (__builtin_expect(!(cond), 0)) \
            __assert_fail("Assertion failed: " #cond "\n"); \
    } while (0)
#endif
