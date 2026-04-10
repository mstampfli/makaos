#pragma once
// unistd.h shim — core functions now in libc.h
#include "../../../libc/libc.h"

// usleep() — microseconds (not yet in libc.h)
static inline int usleep(unsigned int usec) {
    struct timespec req = { (int64_t)(usec / 1000000ULL),
                            (int64_t)((usec % 1000000) * 1000ULL) };
    nanosleep(&req, NULL);
    return 0;
}

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
