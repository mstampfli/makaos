// ── makaos_input.c — native MakaOS input-event library ──────────────
//
// Userland wrapper over /dev/input/event*.  Events are read directly
// in the kernel's 16-byte maka_input_event_t format; no translation
// to Linux's legacy 24-byte struct is performed.
//
// Tier 3 replacement for libinput/libudev-zero — no shim headers,
// no Linux byte-layout compatibility.  Wayland compositors consume
// this via a MakaOS-native input backend.

#include <makaos/input.h>
#include <makaos/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// Kernel ships exactly one input device today (/dev/input/event0 —
// merged keyboard + mouse stream).  When the driver tree grows we'll
// poll /dev/input for eventN nodes; until then, hard-code the list.
static const char* const s_static_devs[] = {
    "/dev/input/event0",
};
enum { s_static_dev_count = (int)(sizeof(s_static_devs) / sizeof(s_static_devs[0])) };

int maka_input_enumerate(char (*paths)[64], int max) {
    if (!paths || max <= 0) { errno = EINVAL; return -1; }
    int n = s_static_dev_count < max ? s_static_dev_count : max;
    for (int i = 0; i < n; i++) {
        const char* src = s_static_devs[i];
        int j = 0;
        while (src[j] && j < 63) { paths[i][j] = src[j]; j++; }
        paths[i][j] = 0;
    }
    return n;
}

int maka_input_open(const char* path, int flags) {
    if (!path) { errno = EINVAL; return -1; }
    // O_RDONLY is the only sensible mode; callers still pass flags for
    // O_NONBLOCK / O_CLOEXEC.
    return open(path, flags | O_RDONLY);
}

int maka_input_read(int fd, maka_input_event_t* buf, int n) {
    if (!buf || n <= 0) { errno = EINVAL; return -1; }
    size_t want = (size_t)n * sizeof(*buf);
    ssize_t got = read(fd, buf, want);
    if (got < 0) return -1;
    // The kernel evdev ring commits whole events — `got` is always a
    // multiple of sizeof(maka_input_event_t).  Return event count.
    return (int)(got / (ssize_t)sizeof(*buf));
}

int maka_input_close(int fd) {
    return close(fd);
}
