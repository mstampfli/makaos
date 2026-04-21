// ── sys_socket.c — <sys/socket.h> extras ────────────────────────────
//
// Basic socket/bind/listen/accept/connect/send/recv lives in libc.c's
// static-inline wrappers.  This file ships the POSIX extras (socketpair,
// sendmsg, recvmsg) as link-resolvable externs for sysroot consumers.

#include <makaos/syscall.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>

int socketpair(int domain, int type, int protocol, int fds[2]) {
    return (int)__syscall_ret(
        syscall4(SYS_SOCKETPAIR,
                 (uint64_t)domain, (uint64_t)type,
                 (uint64_t)protocol, (uint64_t)fds));
}

ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
    return (ssize_t)__syscall_ret(
        syscall3(SYS_SENDMSG, (uint64_t)fd, (uint64_t)msg, (uint64_t)flags));
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
    return (ssize_t)__syscall_ret(
        syscall3(SYS_RECVMSG, (uint64_t)fd, (uint64_t)msg, (uint64_t)flags));
}

// getsockopt — limited kernel support.  SO_PEERCRED is the option
// wayland-server uses to authenticate connecting clients.  On a
// single-user MakaOS we fabricate credentials (uid=0 = root, the
// user running everything); revisit when multi-user lands.
// TODO(scalability-debt-ledger-#4): kernel SYS_GETSOCKOPT + real
// peer-cred tracking on AF_UNIX connect/accept.
int getsockopt(int fd, int level, int optname, void* val, socklen_t* nlen) {
    if (level == SOL_SOCKET && optname == SO_PEERCRED) {
        if (!val || !nlen || *nlen < sizeof(struct ucred)) {
            errno = EINVAL;
            return -1;
        }
        struct ucred* uc = (struct ucred*)val;
        uc->pid = (int32_t)getpid();
        uc->uid = 0;
        uc->gid = 0;
        *nlen = sizeof(*uc);
        return 0;
    }
    // No kernel backing for other options yet.
    (void)fd;
    errno = ENOPROTOOPT;
    return -1;
}
