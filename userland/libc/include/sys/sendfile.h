// sys/sendfile.h - zero-copy-ish file transfer (Linux signature).
//
// MakaOS has no sendfile syscall, so it is implemented over pread/write (see
// libc.c). Semantics match Linux: with a non-NULL offset the input file
// position is left unchanged and *offset is advanced; with NULL the input
// file's own position is used and advanced.

#ifndef _SYS_SENDFILE_H
#define _SYS_SENDFILE_H 1

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count);
ssize_t sendfile64(int out_fd, int in_fd, off64_t* offset, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* sys/sendfile.h */
