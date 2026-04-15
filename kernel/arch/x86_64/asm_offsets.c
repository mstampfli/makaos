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
}
