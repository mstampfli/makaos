#pragma once

// POSIX errno values — identical on kernel and user side.
// Kernel syscalls return -(errno) on failure; libc wrappers convert to
// errno global + -1 return.

#define EPERM        1   // Operation not permitted
#define ENOENT       2   // No such file or directory
#define ESRCH        3   // No such process
#define EINTR        4   // Interrupted system call
#define EIO          5   // I/O error
#define EBADF        9   // Bad file descriptor
#define ECHILD      10   // No child processes
#define EAGAIN      11   // Try again
#define ENOMEM      12   // Out of memory
#define EACCES      13   // Permission denied
#define EEXIST      17   // File exists
#define ENOTDIR     20   // Not a directory
#define EISDIR      21   // Is a directory
#define EINVAL      22   // Invalid argument
#define ENFILE      23   // Too many open files in system
#define ENOSPC      28   // No space left on device
#define ERANGE      34   // Math result not representable
#define ENOTEMPTY   39   // Directory not empty
#define ENOSYS      38   // Function not implemented
#define ENOEXEC      8   // Exec format error
#define EPIPE       32   // Broken pipe
