#pragma once
// unistd.h shim
#include "../../libc.h"

// sleep() — seconds
static inline unsigned int sleep(unsigned int secs) {
    timespec_t req = { (unsigned long long)secs, 0 };
    nanosleep(&req, (timespec_t*)0);
    return 0;
}

// usleep() — microseconds
static inline int usleep(unsigned int usec) {
    timespec_t req = { usec / 1000000ULL, (unsigned long long)(usec % 1000000) * 1000ULL };
    nanosleep(&req, (timespec_t*)0);
    return 0;
}

// getpid / getppid — already in libc.h

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
