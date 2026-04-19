#pragma once
#include "common.h"

// ── kprintf — kernel-side formatted output to serial ────────────────────
// Subset of printf for kernel logging.  Output goes to the debug serial port.
//
// Supported format specifiers:
//   %s   null-terminated string
//   %c   single character
//   %d   signed 32-bit decimal
//   %u   unsigned 32-bit decimal
//   %lu  unsigned 64-bit decimal
//   %x   unsigned 32-bit lowercase hex
//   %X   unsigned 32-bit uppercase hex
//   %lx  unsigned 64-bit lowercase hex
//   %lX  unsigned 64-bit uppercase hex
//   %p   pointer (lowercase hex, 16 digits)
//   %%   literal '%'
//
// No field width, precision, flags, or floating-point support — a kernel
// logger doesn't need them.  New code should prefer kprintf over manual
// serial_puts/serial_hex_u32 chains for readability.

__attribute__((format(printf, 1, 2)))
void kprintf(const char* fmt, ...);

// Like kprintf but holds the serial lock across the entire format+emit.
// The output line cannot byte-interleave with concurrent kprintfs on
// other CPUs — use for lines that must be machine-parseable (test
// harness summaries, crash reports).  Blocks concurrent kprintf on
// other CPUs for the duration of the call; use sparingly.
__attribute__((format(printf, 1, 2)))
void kprintf_atomic(const char* fmt, ...);
