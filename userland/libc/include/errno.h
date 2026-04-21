#ifndef _MAKAOS_ERRNO_H
#define _MAKAOS_ERRNO_H 1

#ifdef __cplusplus
extern "C" {
#endif
// POSIX / Linux-compatible errno values.  The numeric codes must stay in
// sync with kernel/include/errno.h — the kernel returns these verbatim
// via syscall return values (negative = error).

#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define E2BIG         7
#define ENOEXEC       8
#define ENODEV       19
#define EXDEV        18
#define ENXIO         6
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define EWOULDBLOCK  EAGAIN
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EPIPE        32
#define ERANGE       34
#define EDEADLK      35
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ELOOP        40
#define EILSEQ       84
#define ENOTSOCK     88
#define EOPNOTSUPP   95
#define ENOTSUP      EOPNOTSUPP    // Linux alias
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define ECONNRESET  104
#define ENOBUFS     105
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECANCELED   125
#define EOVERFLOW    75    // value too large for defined data type
#define EPROTO       71    // protocol error
#define EPROTONOSUPPORT 93
#define EMSGSIZE     90
#define EADDRNOTAVAIL 99
#define ENETDOWN    100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define EDESTADDRREQ 89
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY    114
#define EINPROGRESS 115
#define ENOPROTOOPT  92
#define ESOCKTNOSUPPORT 94
#define ENOTUNIQ    76
#define ETIME       62    // timer expired

extern int errno;

#ifdef __cplusplus
}
#endif

#endif
