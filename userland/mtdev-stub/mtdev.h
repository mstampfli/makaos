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
