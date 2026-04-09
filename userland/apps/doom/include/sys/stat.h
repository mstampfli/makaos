#pragma once
// sys/stat.h shim
#include "../../../../libc/libc.h"

// mode constants
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRWXO 0007
#define S_ISDIR(m) (0)  // we don't expose full stat mode bits

// mkdir with mode (m_misc.c uses mkdir(path, 0755))
// Our libc mkdir doesn't take a mode, so ignore it.
static inline int mkdir_mode(const char* path, int mode) {
    (void)mode;
    size_t n = __builtin_strlen(path);
    return mkdir(path, n);
}

// Override doom's macro-free mkdir call by providing a real function.
// doom calls mkdir(path, 0755) — intercept via a macro.
#define mkdir(path, mode) mkdir_mode(path, mode)
