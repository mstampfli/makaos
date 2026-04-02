#pragma once
#include "common.h"

/* Calibrate the TSC against PIT channel 2.  Call once early in kmain,
 * after idt_init() and before any tasks run. */
void tsc_init(void);

/* Nanoseconds elapsed since tsc_init() was called. */
uint64_t tsc_read_ns(void);
