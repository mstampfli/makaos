#pragma once
// stdlib.h shim — all core functions now in libc.h/libc.c
#include "../../../libc/libc.h"

static inline long long atoll(const char* s) { return (long long)strtol(s, 0, 10); }
static inline double atof(const char* s) { return strtod(s, NULL); }
static inline int system(const char* cmd) { (void)cmd; return -1; }
// getenv now in libc.h

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

typedef struct { int quot, rem; }       div_t;
typedef struct { long quot, rem; }      ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

static inline div_t div(int num, int den) {
    div_t r; r.quot = num / den; r.rem = num % den; return r;
}
