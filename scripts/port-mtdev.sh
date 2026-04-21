#!/usr/bin/env bash
# ── MakaOS mtdev stub ────────────────────────────────────────────────
#
# mtdev converts legacy multi-touch protocol A (one repeated set of
# ABS_MT_* axes per finger, no slots) into the modern protocol B
# (ABS_MT_SLOT + per-slot tracking IDs).  Our kernel emits protocol B
# natively on touch-capable devices (see evdev.h ABS_MT_SLOT), so
# mtdev just passes events through untouched.  libinput's dependency
# on mtdev is version-gated but otherwise trivial — it uses
# mtdev_new_open() / mtdev_get() / mtdev_delete().  A ~80 LOC stub
# suffices; if we ever grow a protocol-A capable touchscreen driver,
# upstream mtdev's 500 LOC A→B converter can drop in here.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"

log() { printf '[port-mtdev] %s\n' "$*" >&2; }

SRC="$REPO_ROOT/userland/mtdev-stub"
mkdir -p "$SRC"

cat > "$SRC/mtdev.h" << 'EOF'
/* MakaOS mtdev pass-through stub.  API matches upstream mtdev 1.1.6
 * so libinput links unmodified.  Since our kernel emits protocol B
 * events natively, mtdev_get() simply forwards reads. */
#ifndef MTDEV_H
#define MTDEV_H

#include <stddef.h>
#include <stdint.h>

#define MTDEV_ABS_SIZE   11
#define MTDEV_MAX_SLOTS  32

struct input_event;
struct input_absinfo;

/* libinput pokes mtdev->caps.slot.value on protocol-A devices
 * (evdev-fallback.c fallback_init_slots) to pick the active slot.
 * Our kernel emits protocol B, so evdev_need_mtdev() returns false
 * and this field is never actually read — but the type must be
 * complete for compilation.  Mirror upstream mtdev 1.1.6 layout. */
struct mtdev_caps_slot {
    int has_mt;
    int identifier;
    int minimum;
    int maximum;
    int value;
};
struct mtdev_caps {
    int abs_required;
    struct mtdev_caps_slot slot;
};

struct mtdev {
    int state;       /* opaque — used by protocol-A path we omit */
    struct mtdev_caps caps;
};

#ifdef __cplusplus
extern "C" {
#endif

struct mtdev *mtdev_new(void);
int  mtdev_init(struct mtdev *dev);
int  mtdev_open(struct mtdev *dev, int fd);
struct mtdev *mtdev_new_open(int fd);
void mtdev_close(struct mtdev *dev);
void mtdev_delete(struct mtdev *dev);

/* Pull events from the file descriptor.  Returns number read, 0 on
 * EAGAIN, -errno on error. */
int mtdev_get(struct mtdev *dev, int fd, struct input_event *evs, int ev_max);

int  mtdev_has_mt_event(const struct mtdev *dev, int mt_code);
int  mtdev_has_slot(const struct mtdev *dev);
int  mtdev_idle(const struct mtdev *dev, int fd, int timeout_ms);
int  mtdev_empty(const struct mtdev *dev);
void mtdev_put(struct mtdev *dev, const struct input_event *ev);
void mtdev_put_event(struct mtdev *dev, const struct input_event *ev);
void mtdev_fetch_event(struct mtdev *dev, struct input_event *ev);
/* mtdev_get_event — the active-queue variant used by libinput's
 * evdev.c when draining buffered events after a SYN_REPORT.  Since
 * our kernel emits protocol B and the queue is always empty, write
 * a zeroed SYN_REPORT so the caller terminates cleanly. */
void mtdev_get_event(struct mtdev *dev, struct input_event *ev);
/* mtdev_close_delete — combined close+delete convenience wrapper. */
void mtdev_close_delete(struct mtdev *dev);
void mtdev_configure(struct mtdev *dev);

#ifdef __cplusplus
}
#endif

#endif /* MTDEV_H */
EOF

cat > "$SRC/mtdev.c" << 'EOF'
/* Pass-through stub: every function either forwards to read() or
 * reports "no protocol-A translation needed".  libinput only checks
 * return codes + reads evs; it doesn't introspect internal state. */
#include "mtdev.h"
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

struct mtdev *mtdev_new(void) {
    struct mtdev *d = calloc(1, sizeof(*d));
    return d;
}

int mtdev_init(struct mtdev *dev) { (void)dev; return 0; }
int mtdev_open(struct mtdev *dev, int fd) { (void)dev; (void)fd; return 0; }

struct mtdev *mtdev_new_open(int fd) {
    (void)fd;
    return mtdev_new();
}

void mtdev_close(struct mtdev *dev) { (void)dev; }

void mtdev_delete(struct mtdev *dev) { free(dev); }

int mtdev_get(struct mtdev *dev, int fd, struct input_event *evs, int ev_max) {
    (void)dev;
    extern long read(int, void*, unsigned long);
    /* Our kernel emits protocol B directly — pass through. */
    long r = read(fd, evs, (unsigned long)ev_max * 24);
    if (r < 0) return -1;
    return (int)(r / 24);
}

int mtdev_has_mt_event(const struct mtdev *dev, int mt_code) {
    (void)dev; (void)mt_code;
    return 1;  /* claim support — kernel emits all ABS_MT_* natively */
}

int mtdev_has_slot(const struct mtdev *dev) { (void)dev; return 1; }

int mtdev_idle(const struct mtdev *dev, int fd, int timeout_ms) {
    (void)dev;
    struct pollfd p = { .fd = fd, .events = 1 /*POLLIN*/ };
    int r = poll(&p, 1, timeout_ms);
    return r == 0;  /* idle if poll timed out */
}

int mtdev_empty(const struct mtdev *dev) { (void)dev; return 1; }

void mtdev_put(struct mtdev *dev, const struct input_event *ev) {
    (void)dev; (void)ev;
}

void mtdev_put_event(struct mtdev *dev, const struct input_event *ev) {
    (void)dev; (void)ev;
}

void mtdev_fetch_event(struct mtdev *dev, struct input_event *ev) {
    (void)dev; (void)ev;
}

void mtdev_get_event(struct mtdev *dev, struct input_event *ev) {
    (void)dev;
    /* Queue is always empty on protocol-B devices.  Zero the event
     * so any stray caller sees a benign SYN_REPORT. */
    if (ev) {
        char *p = (char *)ev;
        for (unsigned i = 0; i < 24; i++) p[i] = 0;
    }
}

void mtdev_close_delete(struct mtdev *dev) {
    mtdev_close(dev);
    mtdev_delete(dev);
}

void mtdev_configure(struct mtdev *dev) { (void)dev; }
EOF

# Build static archive.
OBJ="$BUILD_DIR/mtdev_objs"
mkdir -p "$OBJ"
"$CROSS_CC" --sysroot="$SYSROOT" -nostdinc \
    -isystem "$SYSROOT/usr/include" \
    -isystem "$REPO_ROOT/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/include" \
    -I "$SRC" \
    -O2 -fPIC -Wall -c "$SRC/mtdev.c" -o "$OBJ/mtdev.o"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libmtdev.a" "$OBJ/mtdev.o"

# mtdev-plumbing.h is the internal-only header libinput reaches into.
# The only symbol it exports we actually need is mtdev_is_absmt_ranged(),
# a predicate upstream uses to decide MT protocol translation.  Our
# stub always returns 0 (we emit protocol B, no ranging needed).
cat > "$SRC/mtdev-plumbing.h" << 'EOF'
#ifndef MTDEV_PLUMBING_H
#define MTDEV_PLUMBING_H
#include "mtdev.h"
static inline int mtdev_is_absmt_ranged(int abs_code) { (void)abs_code; return 0; }
#endif
EOF

# Install headers + pkg-config.
mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib/pkgconfig"
cp "$SRC/mtdev.h"          "$SYSROOT/usr/include/mtdev.h"
cp "$SRC/mtdev-plumbing.h" "$SYSROOT/usr/include/mtdev-plumbing.h"
cat > "$SYSROOT/usr/lib/pkgconfig/mtdev.pc" << EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: mtdev
Description: MakaOS pass-through mtdev (protocol A→B no-op)
Version: 1.1.6
Libs: -L\${libdir} -lmtdev
Cflags: -I\${includedir}
EOF

log "installed $(ls -la $SYSROOT/usr/lib/libmtdev.a)"
