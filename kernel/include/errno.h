#pragma once

// POSIX errno values — identical on kernel and user side.
// Kernel syscalls return -(errno) on failure; libc wrappers convert to
// errno global + -1 return.

#define EPERM        1   // Operation not permitted
#define ENOENT       2   // No such file or directory
#define ESRCH        3   // No such process
#define EINTR        4   // Interrupted system call
#define EIO          5   // I/O error
#define ENOEXEC      8   // Exec format error
#define EBADF        9   // Bad file descriptor
#define ECHILD      10   // No child processes
#define EAGAIN      11   // Try again
#define ENOMEM      12   // Out of memory
#define EACCES      13   // Permission denied
#define EFAULT      14   // Bad address
#define EBUSY       16   // Device or resource busy
#define EEXIST      17   // File exists
#define ENOTDIR     20   // Not a directory
#define EISDIR      21   // Is a directory
#define EINVAL      22   // Invalid argument
#define ENFILE      23   // Too many open files in system
#define EMFILE      24   // Too many open files (per-process)
#define ENOTTY      25   // Not a typewriter (not a tty)
#define EFBIG       27   // File too large
#define ENOSPC      28   // No space left on device
#define ESPIPE      29   // Illegal seek
#define EPIPE       32   // Broken pipe
#define ERANGE      34   // Math result not representable
#define EDEADLK     35   // Resource deadlock avoided
#define ENAMETOOLONG 36  // File name too long
#define ENOSYS      38   // Function not implemented
#define ENOTEMPTY   39   // Directory not empty
#define ELOOP       40   // Too many symbolic links
#define EWOULDBLOCK EAGAIN // Operation would block
#define ENOMSG      42   // No message of desired type
#define ENOTSOCK    88   // Socket operation on non-socket
#define EDESTADDRREQ 89  // Destination address required
#define ENOPROTOOPT 92   // Protocol not available
#define EOPNOTSUPP  95   // Operation not supported on socket
#define EAFNOSUPPORT 97  // Address family not supported
#define EADDRINUSE  98   // Address already in use
#define ECONNABORTED 103 // Connection aborted
#define ECONNRESET  104 // Connection reset by peer
#define ENOBUFS     105 // No buffer space available
#define EISCONN     106 // Transport endpoint is already connected
#define ENOTCONN    107 // Transport endpoint is not connected
#define ETIMEDOUT   110 // Connection timed out
#define ECONNREFUSED 111 // Connection refused
#define ENETDOWN     100 // Network is down
#define ENETUNREACH  101 // Network unreachable
#define EHOSTUNREACH 113 // No route to host
#define EINPROGRESS  115 // Operation now in progress (nonblocking connect)
#define EALREADY     114 // Operation already in progress
#define ECANCELED    125 // Operation canceled (io_uring IO_LINK cascade)
