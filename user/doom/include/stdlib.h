#pragma once
// stdlib.h shim — redirects to our libc
#include "../../libc.h"

// Additional stdlib functions provided by libc.c
static inline void abort(void) { _exit(134); }
static inline long long atoll(const char* s) { return (long long)strtol(s, 0, 10); }
static inline double atof(const char* s) { (void)s; return 0.0; } // minimal stub
static inline int system(const char* cmd) { (void)cmd; return -1; }
static inline char* getenv(const char* name) { (void)name; return (char*)0; }

// abs/labs/llabs already declared in libc.h

// EXIT_SUCCESS / EXIT_FAILURE
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

// div_t and friends (unused by Doom core but need to be declared)
typedef struct { int quot, rem; }       div_t;
typedef struct { long quot, rem; }      ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

static inline div_t div(int num, int den) {
    div_t r; r.quot = num / den; r.rem = num % den; return r;
}
