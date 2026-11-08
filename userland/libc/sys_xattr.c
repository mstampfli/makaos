// sys_xattr.c - <sys/xattr.h>.
//
// MakaOS filesystems store no extended attributes, so every accessor reports
// ENOTSUP. get/list return -1 (callers see "no xattrs"); set/remove fail the
// same way. Portable code (OpenJDK's UnixNativeDispatcher) checks errno and
// disables its xattr paths.

#include <sys/xattr.h>
#include <errno.h>

ssize_t getxattr(const char* path, const char* name, void* value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t lgetxattr(const char* path, const char* name, void* value, size_t size) {
    (void)path; (void)name; (void)value; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t fgetxattr(int fd, const char* name, void* value, size_t size) {
    (void)fd; (void)name; (void)value; (void)size;
    errno = ENOTSUP;
    return -1;
}

int setxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOTSUP;
    return -1;
}

int lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
    (void)path; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOTSUP;
    return -1;
}

int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
    (void)fd; (void)name; (void)value; (void)size; (void)flags;
    errno = ENOTSUP;
    return -1;
}

ssize_t listxattr(const char* path, char* list, size_t size) {
    (void)path; (void)list; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t llistxattr(const char* path, char* list, size_t size) {
    (void)path; (void)list; (void)size;
    errno = ENOTSUP;
    return -1;
}

ssize_t flistxattr(int fd, char* list, size_t size) {
    (void)fd; (void)list; (void)size;
    errno = ENOTSUP;
    return -1;
}

int removexattr(const char* path, const char* name) {
    (void)path; (void)name;
    errno = ENOTSUP;
    return -1;
}

int lremovexattr(const char* path, const char* name) {
    (void)path; (void)name;
    errno = ENOTSUP;
    return -1;
}

int fremovexattr(int fd, const char* name) {
    (void)fd; (void)name;
    errno = ENOTSUP;
    return -1;
}
