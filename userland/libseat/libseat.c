// MakaOS native libseat stub.  We have no session management so every
// call succeeds and the seat is permanently enabled.  Device opens
// route directly to open(2) with the caller's path; revoke semantics
// are unused because nothing ever revokes.

#include "libseat.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/eventfd.h>

// TODO(scalability-debt-ledger-#1): replace this stub with a real seat
// broker client.  The current always-grant model is fine for single-user
// MakaOS; revoke/session-switch semantics block on kernel capability
// work (SECURITY_V2.md).  fd_table below is grow-on-demand so the
// 64-device ceiling in the original stub is already gone.
struct libseat {
    int    event_fd;          // eventfd — never signalled, poll() compat
    int    next_device_id;
    int*   fd_table;          // dynamically grown: fd_table[id-1] = fd (or -1)
    int    fd_table_cap;      // allocated slots
    const struct libseat_seat_listener* listener;
    void*  userdata;
};

static int __fd_table_ensure(struct libseat* s, int need_id) {
    if (need_id <= s->fd_table_cap) return 0;
    int new_cap = s->fd_table_cap ? s->fd_table_cap : 16;
    while (new_cap < need_id) new_cap *= 2;
    int* nt = (int*)realloc(s->fd_table, (size_t)new_cap * sizeof(int));
    if (!nt) { errno = ENOMEM; return -1; }
    for (int i = s->fd_table_cap; i < new_cap; i++) nt[i] = -1;
    s->fd_table = nt;
    s->fd_table_cap = new_cap;
    return 0;
}

static const char s_seat_name[] = "seat0";

struct libseat* libseat_open_seat(const struct libseat_seat_listener* listener, void* userdata) {
    if (!listener || !listener->enable_seat || !listener->disable_seat) {
        errno = EINVAL;
        return 0;
    }
    struct libseat* s = (struct libseat*)calloc(1, sizeof(*s));
    if (!s) { errno = ENOMEM; return 0; }
    s->event_fd       = eventfd(0, 0);
    s->next_device_id = 1;
    s->listener       = listener;
    s->userdata       = userdata;
    // fd_table starts empty; grows on demand in libseat_open_device.
    listener->enable_seat(s, userdata);   // MakaOS is always active
    return s;
}

int libseat_disable_seat(struct libseat* seat) {
    (void)seat;
    return 0;      // no-op; nothing ever disables us
}

int libseat_close_seat(struct libseat* seat) {
    if (!seat) return 0;
    for (int i = 0; i < seat->fd_table_cap; i++)
        if (seat->fd_table[i] >= 0) close(seat->fd_table[i]);
    free(seat->fd_table);
    if (seat->event_fd >= 0) close(seat->event_fd);
    free(seat);
    return 0;
}

int libseat_open_device(struct libseat* seat, const char* path, int* fd) {
    if (!seat || !path || !fd) { errno = EINVAL; return -1; }
    // Always open non-blocking.  libinput + wlroots both assume the
    // seat hands back an O_NONBLOCK fd (on Linux, systemd-logind/seatd
    // pass it that way over the FD-passing socket), and wlroots'
    // libinput_open_restricted discards the caller's flags argument
    // rather than applying it.  On DRM this only affects the page-flip
    // event loop, which compositors drive via poll() anyway.
    int real = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (real < 0) real = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (real < 0) return -1;
    int id;
    // Search for a free slot; grow if none.
    for (id = 1; id <= seat->fd_table_cap; id++) {
        if (seat->fd_table[id - 1] == -1) break;
    }
    if (__fd_table_ensure(seat, id) < 0) { close(real); return -1; }
    seat->fd_table[id - 1] = real;
    *fd = real;
    return id;
}

int libseat_close_device(struct libseat* seat, int device_id) {
    if (!seat || device_id < 1 || device_id > seat->fd_table_cap) {
        errno = EINVAL; return -1;
    }
    int slot = device_id - 1;
    if (seat->fd_table[slot] < 0) { errno = ENOENT; return -1; }
    close(seat->fd_table[slot]);
    seat->fd_table[slot] = -1;
    return 0;
}

const char* libseat_seat_name(struct libseat* seat) { (void)seat; return s_seat_name; }

int libseat_switch_session(struct libseat* seat, int session) {
    (void)seat; (void)session;
    return 0;   // no VTs to switch to
}

int libseat_get_fd(struct libseat* seat) {
    if (!seat) { errno = EINVAL; return -1; }
    return seat->event_fd;
}

int libseat_dispatch(struct libseat* seat, int timeout) {
    (void)seat; (void)timeout;
    return 0;   // no events ever fire
}

void libseat_set_log_handler(libseat_log_func fn) { (void)fn; }
void libseat_set_log_level(enum libseat_log_level level) { (void)level; }
