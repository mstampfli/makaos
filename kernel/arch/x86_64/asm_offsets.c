// Generate compile-time offsets into NASM-consumable defines.
//
// This file is NOT linked into the kernel.  The build system compiles it
// with `-S` so clang emits an assembly listing; a sed/awk pass then
// rewrites the `->NAME $val` lines into a NASM `%define NAME val` header
// that syscall_entry.asm (and any other .asm file that needs struct
// offsets) can `%include`.
//
// The mechanism lets asm code reach inside C structs without ever
// hardcoding an offset — add a field, rebuild, the offset updates
// automatically.  Same trick the Linux kernel uses (arch/x86/kernel/
// asm-offsets.c + scripts/Kbuild.asm-offsets).

#include "cpu.h"
#include "tss.h"

// Emit one .ascii line per define.  The assembly output looks like:
//     .ascii  "@@@ASMDEF CPU_TSS_RSP0 796"
// A post-processing awk rewrites it into `%define CPU_TSS_RSP0 796`.
// We use .ascii (instead of a Linux-style `->` comment) because clang's
// integrated assembler validates inline asm; .ascii is a real directive
// so the inline asm assembles cleanly, and the strings disappear into
// an unreferenced section that we never link into kernel.elf.
#define DEFINE(sym, val) \
    __asm__ volatile(".ascii \"@@@ASMDEF " #sym " %c0\"" : : "i" ((long)(val)))

void asm_offsets(void);
void asm_offsets(void) {
    DEFINE(CPU_TSS_RSP0,
           __builtin_offsetof(cpu_t, tss) + __builtin_offsetof(tss_t, rsp[0]));

    // Per-CPU syscall / signal / exec scratch — see cpu_t commentary
    // for the race this fixes.
    DEFINE(CPU_SC_USER_RSP,    __builtin_offsetof(cpu_t, syscall_user_rsp));
    DEFINE(CPU_SC_USER_RIP,    __builtin_offsetof(cpu_t, syscall_user_rip));
    DEFINE(CPU_SC_USER_RFLAGS, __builtin_offsetof(cpu_t, syscall_user_rflags));
    DEFINE(CPU_SC_USER_RBP,    __builtin_offsetof(cpu_t, syscall_user_rbp));
    DEFINE(CPU_SC_USER_RBX,    __builtin_offsetof(cpu_t, syscall_user_rbx));
    DEFINE(CPU_SC_USER_R12,    __builtin_offsetof(cpu_t, syscall_user_r12));
    DEFINE(CPU_SC_USER_R13,    __builtin_offsetof(cpu_t, syscall_user_r13));
    DEFINE(CPU_SC_USER_R14,    __builtin_offsetof(cpu_t, syscall_user_r14));
    DEFINE(CPU_SC_USER_R15,    __builtin_offsetof(cpu_t, syscall_user_r15));
    DEFINE(CPU_SC_ARG5,        __builtin_offsetof(cpu_t, syscall_arg5));
    DEFINE(CPU_SC_ARG6,        __builtin_offsetof(cpu_t, syscall_arg6));
    DEFINE(CPU_SIG_DELIVER,    __builtin_offsetof(cpu_t, signal_deliver));
    DEFINE(CPU_SIG_IN_SYSCALL, __builtin_offsetof(cpu_t, signal_in_syscall));
    DEFINE(CPU_SIG_RDI,        __builtin_offsetof(cpu_t, signal_rdi));
    DEFINE(CPU_EXEC_REQUESTED, __builtin_offsetof(cpu_t, exec_requested));
    DEFINE(CPU_EXEC_ENTRY,     __builtin_offsetof(cpu_t, exec_entry));
    DEFINE(CPU_EXEC_RSP,       __builtin_offsetof(cpu_t, exec_rsp));
    DEFINE(CPU_EXEC_PML4,      __builtin_offsetof(cpu_t, exec_pml4));
}
