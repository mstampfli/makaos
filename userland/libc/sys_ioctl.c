// ── sys_ioctl.c — <sys/ioctl.h> ─────────────────────────────────────
//
// Variadic ioctl — all real callers (DRM/KMS, evdev, TTY) pass a single
// trailing pointer argument, so we marshal exactly one `void*` through
// the ABI.

#include <makaos/syscall.h>
#include <sys/ioctl.h>
#include <stdarg.h>

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    return (int)__syscall_ret(syscall3(SYS_IOCTL, (uint64_t)fd, (uint64_t)req, (uint64_t)arg));
}
