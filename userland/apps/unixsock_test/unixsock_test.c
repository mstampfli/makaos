/*
 * unixsock_test -- isolate which syscall wl_display_add_socket_auto() hangs on.
 *
 * It replays the sequence wayland-server's _wl_display_add_socket() performs:
 * create a lockfile under XDG_RUNTIME_DIR (/tmp), then
 * socket(AF_UNIX)/bind(path)/listen(), then register the fd with epoll
 * (wl_event_loop_add_fd).  flock is a proven no-op on MakaOS so it is skipped.
 * A sentinel is emitted AFTER each step via an unknown-ioctl req number the drm
 * layer pr_warns to serial (app stdout goes to the VGA console, not mirrored).
 * The LAST sentinel on serial is the step whose syscall hangs.
 *
 *   A0 start  A1 opened lock  A3 socket  A4 bind  A5 listen
 *   A6 epoll_create  A7 epoll_ctl  AF all returned
 *
 * Uses only libc.h (MakaOS umbrella header): pulling <sys/socket.h> etc. drags
 * in the toolchain stdint and conflicts with libc.h's 64-bit typedefs.
 */
#include "libc.h"

static int g_dbgfd = -1;
static void dbg(unsigned code) {
    if (g_dbgfd >= 0) ioctl(g_dbgfd, 0xC1000000u | (code & 0xFFu), (void *)0);
}

int main(void) {
    g_dbgfd = open("/dev/dri/renderD128", O_RDWR, 0);
    if (g_dbgfd < 0) g_dbgfd = open("/dev/dri/card0", O_RDWR, 0);
    dbg(0xA0);

    int lock = open("/tmp/unixsock_test.lock", O_CREAT | O_CLOEXEC | O_RDWR, 0660);
    dbg(0xA1);
    (void)lock;

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    dbg(0xA3);

    sockaddr_un_t addr;
    __builtin_memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    __builtin_memcpy(addr.sun_path, "/tmp/unixsock_test.sock",
                     sizeof("/tmp/unixsock_test.sock"));
    unlink(addr.sun_path);
    int br = bind(s, (struct sockaddr *)&addr, sizeof(addr));
    dbg(0xA4);

    int lr = listen(s, 128);
    dbg(0xA5);

    int ep = epoll_create1(0);
    dbg(0xA6);

    epoll_event_t ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = s;
    epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev);
    dbg(0xA7);

    dbg(0xAF);   /* every step returned -- none of the syscalls hung */
    printf("unixsock_test: PASS s=%d bind=%d listen=%d ep=%d\n", s, br, lr, ep);
    return 0;
}
