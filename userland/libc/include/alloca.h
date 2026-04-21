#ifndef _MAKAOS_ALLOCA_H
#define _MAKAOS_ALLOCA_H 1

// alloca — stack allocation.  Compiler builtin on gcc/clang.  We use
// __builtin_alloca unconditionally; no separate runtime support needed.
#define alloca(size) __builtin_alloca(size)

#endif
