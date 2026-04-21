#!/usr/bin/env bash
# ── MakaOS native libseat — always-grant stub ──────────────────────────
#
# MakaOS has no VTs, no logind, no session switching.  Every device
# open is granted immediately.  We ship just the public libseat.h API
# shape so wlroots + anything else expecting libseat links cleanly and
# gets "enabled, always active" semantics.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

log() { printf '[port-libseat] %s\n' "$*" >&2; }

SRC_DIR="$REPO_ROOT/userland/libseat"
mkdir -p "$SRC_DIR"

# ── libseat.h — upstream-compatible API shape ──────────────────────
cat > "$SRC_DIR/libseat.h" << 'EOF'
#ifndef _LIBSEAT_H
#define _LIBSEAT_H

#include <stdarg.h>

struct libseat;

struct libseat_seat_listener {
    void (*enable_seat)(struct libseat* seat, void* userdata);
    void (*disable_seat)(struct libseat* seat, void* userdata);
};

struct libseat* libseat_open_seat(const struct libseat_seat_listener* listener, void* userdata);
int             libseat_disable_seat(struct libseat* seat);
int             libseat_close_seat(struct libseat* seat);

int         libseat_open_device(struct libseat* seat, const char* path, int* fd);
int         libseat_close_device(struct libseat* seat, int device_id);
const char* libseat_seat_name(struct libseat* seat);

int libseat_switch_session(struct libseat* seat, int session);
int libseat_get_fd(struct libseat* seat);
int libseat_dispatch(struct libseat* seat, int timeout);

enum libseat_log_level {
    LIBSEAT_LOG_LEVEL_SILENT = 0,
    LIBSEAT_LOG_LEVEL_ERROR  = 1,
    LIBSEAT_LOG_LEVEL_INFO   = 2,
    LIBSEAT_LOG_LEVEL_DEBUG  = 3,
    LIBSEAT_LOG_LEVEL_LAST,
};
typedef void (*libseat_log_func)(enum libseat_log_level level, const char* fmt, va_list args);
void libseat_set_log_handler(libseat_log_func fn);
void libseat_set_log_level(enum libseat_log_level level);

#endif
EOF

# ── libseat.c — always-grant stub ──────────────────────────────────
cat > "$SRC_DIR/libseat.c" << 'EOF'
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
    int real = open(path, O_RDWR | O_CLOEXEC);
    if (real < 0) real = open(path, O_RDONLY | O_CLOEXEC);
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
EOF

# ── Compile + archive ──────────────────────────────────────────────
objdir="$BUILD_DIR/libseat_objs"
mkdir -p "$objdir"
log "compiling libseat"
"$CROSS_CC" -O2 -fPIC -Wall \
    --sysroot="$SYSROOT" -nostdinc -isystem "$SYSROOT/usr/include" \
    -c "$SRC_DIR/libseat.c" -o "$objdir/libseat.o"
rm -f "$SYSROOT/usr/lib/libseat.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libseat.a" "$objdir/libseat.o"

# ── Install header + pkg-config ────────────────────────────────────
cp "$SRC_DIR/libseat.h" "$SYSROOT/usr/include/libseat.h"
mkdir -p "$SYSROOT/usr/lib/pkgconfig"
cat > "$SYSROOT/usr/lib/pkgconfig/libseat.pc" << 'EOF'
prefix=/usr
libdir=${prefix}/lib
includedir=${prefix}/include

Name: libseat
Description: MakaOS native libseat (always-grant stub)
Version: 0.9.1
Libs: -L${libdir} -lseat
Cflags: -I${includedir}
EOF

log "libseat.a: $(stat -c%s "$SYSROOT/usr/lib/libseat.a") bytes"
log "done"
