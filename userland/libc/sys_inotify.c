// sys_inotify.c - <sys/inotify.h>.
//
// MakaOS has no inotify subsystem. Every entry point fails with ENOSYS so
// portable callers detect the absence and degrade gracefully -- OpenJDK's
// LinuxWatchService, for instance, catches the init failure and falls back to
// its PollingWatchService.

#include <sys/inotify.h>
#include <errno.h>

int inotify_init(void) {
    errno = ENOSYS;
    return -1;
}

int inotify_init1(int flags) {
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int inotify_add_watch(int fd, const char* pathname, uint32_t mask) {
    (void)fd; (void)pathname; (void)mask;
    errno = ENOSYS;
    return -1;
}

int inotify_rm_watch(int fd, int wd) {
    (void)fd; (void)wd;
    errno = ENOSYS;
    return -1;
}
