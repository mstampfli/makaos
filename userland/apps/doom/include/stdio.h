#pragma once
// stdio.h shim — everything now lives in our libc; just pull it in.
#include "../../../libc/stdio.h"
#include "../../../libc/libc.h"

// remove() — alias for unlink
static inline int remove(const char* path) { return unlink(path); }

// scanf stubs (not used by doom core)
static inline int scanf(const char* fmt, ...) { (void)fmt; return 0; }
static inline int fscanf(FILE* f, const char* fmt, ...) { (void)f; (void)fmt; return 0; }
