// ssp.c - Stack Smashing Protector runtime (-fstack-protector).
//
// MakaOS's gcc emits the *global*-guard form of the protector: the function
// prologue loads __stack_chk_guard and the epilogue compares the on-stack copy
// against it, calling __stack_chk_fail on a mismatch. (We use the global guard
// rather than the %fs:0x28 TLS slot because the TCB canary slot is not part of
// the MakaOS TLS ABI.) Code compiled with -fstack-protector -- e.g. OpenJDK's
// libjli/libjvm -- references these two symbols, so libc must define them.
//
// This file follows the libc-internal convention: include only "libc.h" (which
// carries libc's own type and prototype copies), never the sysroot headers.

#include "libc.h"

// The canary. Seeded to a terminator value (a NUL low byte defeats string
// overruns that would otherwise walk past it) and re-randomized per process at
// startup so the value is not shared across processes.
uintptr_t __stack_chk_guard = (uintptr_t)0xff0a0d0000000000ULL;

// Randomize the guard before main runs. This function MUST NOT be stack
// protected itself: it mutates the very global its own epilogue would check
// against, which would otherwise fault on return.
__attribute__((constructor, no_stack_protector))
static void __libc_ssp_init(void) {
    uintptr_t g = 0;
    getrandom(&g, sizeof(g), 0);
    // Force a NUL in the low byte so a string overflow cannot forge the canary
    // by overwriting up to (but not including) it.
    g &= ~(uintptr_t)0xff;
    __stack_chk_guard = g;
}

// Called when a corrupted canary is detected. Overflow already happened, so the
// only safe action is to report and terminate without unwinding.
__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void) {
    static const char msg[] = "*** stack smashing detected ***: terminated\n";
    write(2, msg, sizeof(msg) - 1);
    abort();
    __builtin_unreachable();
}
