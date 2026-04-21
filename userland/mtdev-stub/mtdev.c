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
