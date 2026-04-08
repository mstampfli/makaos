#pragma once
// unistd.h shim
#include "../../libc.h"

// access() — stub: always returns -1 (not found) unless it's a try-open.
static inline int access(const char* path, int mode) {
    (void)mode;
    // Try to open and immediately close.
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

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
