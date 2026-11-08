#ifndef _MAKAOS_SYS_XATTR_H
#define _MAKAOS_SYS_XATTR_H 1

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// setxattr/lsetxattr/fsetxattr flags.
#define XATTR_CREATE  1
#define XATTR_REPLACE 2

// MakaOS's filesystems carry no extended attributes. These follow the Linux
// prototypes so portable code (OpenJDK's UnixNativeDispatcher) compiles; at
// runtime they report ENOTSUP and callers treat xattrs as unavailable.
ssize_t getxattr(const char* path, const char* name, void* value, size_t size);
ssize_t lgetxattr(const char* path, const char* name, void* value, size_t size);
ssize_t fgetxattr(int fd, const char* name, void* value, size_t size);

int setxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags);

ssize_t listxattr(const char* path, char* list, size_t size);
ssize_t llistxattr(const char* path, char* list, size_t size);
ssize_t flistxattr(int fd, char* list, size_t size);

int removexattr(const char* path, const char* name);
int lremovexattr(const char* path, const char* name);
int fremovexattr(int fd, const char* name);

#ifdef __cplusplus
}
#endif

#endif
