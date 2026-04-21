// ── MakaOS native wlroots backend ───────────────────────────────────
//
// Replaces the libinput backend on Linux distributions.  Opens
// /dev/input/event0 directly, reads maka_input_event_t (16 B,
// ns timestamp) from the kernel, and translates each event into
// wlr_keyboard_key_event / wlr_pointer_{motion,button}_event signals.
//
// This backend provides INPUT ONLY.  Outputs come from the DRM
// backend (wlroots' built-in), combined via wlr_multi_backend.
//
// TODO(scalability-debt-ledger-#3): when the kernel exposes per-device
// evdev nodes (event0 = keyboard, event1 = mouse, …), open each node
// separately and bind it to its own wlr_input_device.

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <makaos/input.h>
#include <wlr/util/log.h>
#include "native.h"

static struct wlr_native_backend* backend_from_wlr(struct wlr_backend* wlr_b) {
    return wl_container_of(wlr_b, (struct wlr_native_backend*)0, backend);
}

// ── fd dispatcher ────────────────────────────────────────────────
// wl_event_loop calls this whenever /dev/input/event0 has bytes ready.
// Read up to 32 events in one go; translate each; keep reading while
// the kernel has more queued.
static int on_input_readable(int fd, uint32_t mask, void* data) {
    (void)mask;
    struct wlr_native_backend* b = (struct wlr_native_backend*)data;
    maka_input_event_t ring[32];
    for (;;) {
        int n = maka_input_read(fd, ring, 32);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN) {
                wlr_log(WLR_ERROR, "native: input read error: %d", errno);
            }
            break;
        }
        for (int i = 0; i < n; i++) {
            maka_input_event_t* e = &ring[i];
            switch (e->type) {
            case MAKA_EV_SYN:
                // Kernel doesn't emit SYN today; no-op.
                break;
            case MAKA_EV_KEY:
                if (e->code >= BTN_MOUSE && e->code <= BTN_MIDDLE)
                    native_dispatch_button(b, e->code, e->value, e->time_ns);
                else
                    native_dispatch_key(b, e->code, e->value, e->time_ns);
                break;
            case MAKA_EV_REL:
                native_dispatch_rel(b, e->code, e->value, e->time_ns);
                break;
            default:
                break;
            }
        }
    }
    return 0;
}

// ── backend_impl ─────────────────────────────────────────────────

static bool backend_start(struct wlr_backend* wlr_b) {
    struct wlr_native_backend* b = backend_from_wlr(wlr_b);
    if (b->started) return true;
    wlr_log(WLR_INFO, "Starting MakaOS native backend");

    // Open /dev/input/event0 non-blocking — we drain via the event
    // loop callback, so blocking reads would stall the compositor.
    b->input_fd = maka_input_open("/dev/input/event0", O_NONBLOCK | O_CLOEXEC);
    if (b->input_fd < 0) {
        wlr_log(WLR_ERROR, "native: cannot open /dev/input/event0: %d", errno);
        return false;
    }

    b->input_source = wl_event_loop_add_fd(b->event_loop, b->input_fd,
        WL_EVENT_READABLE, on_input_readable, b);
    if (!b->input_source) {
        wlr_log(WLR_ERROR, "native: wl_event_loop_add_fd failed");
        close(b->input_fd); b->input_fd = -1;
        return false;
    }

    // Register our keyboard + pointer with wlroots now that the fd
    // is being watched.  Order matters: keyboard first (compositor
    // focus-by-default convention).
    wl_signal_emit_mutable(&b->backend.events.new_input, &b->keyboard.base);
    wl_signal_emit_mutable(&b->backend.events.new_input, &b->pointer.base);

    b->started = true;
    return true;
}

static void backend_destroy(struct wlr_backend* wlr_b) {
    if (!wlr_b) return;
    struct wlr_native_backend* b = backend_from_wlr(wlr_b);

    wlr_backend_finish(wlr_b);

    if (b->input_source) wl_event_source_remove(b->input_source);
    if (b->input_fd >= 0) close(b->input_fd);

    wlr_keyboard_finish(&b->keyboard);
    wlr_pointer_finish (&b->pointer);

    wl_list_remove(&b->event_loop_destroy.link);
    free(b);
}

static uint32_t get_buffer_caps(struct wlr_backend* wlr_b) {
    (void)wlr_b;
    // Input-only backend — no buffer path.
    return 0;
}

static const struct wlr_backend_impl backend_impl = {
    .start           = backend_start,
    .destroy         = backend_destroy,
    .get_buffer_caps = get_buffer_caps,
};

static void handle_event_loop_destroy(struct wl_listener* listener, void* data) {
    (void)data;
    struct wlr_native_backend* b = wl_container_of(listener, b, event_loop_destroy);
    backend_destroy(&b->backend);
}

// ── public factory ───────────────────────────────────────────────

struct wlr_backend* wlr_native_backend_create(struct wl_event_loop* loop) {
    wlr_log(WLR_INFO, "Creating MakaOS native backend");
    struct wlr_native_backend* b = calloc(1, sizeof(*b));
    if (!b) return 0;

    b->input_fd = -1;
    b->event_loop = loop;
    wlr_backend_init(&b->backend, &backend_impl);

    if (!native_keyboard_init(b) || !native_pointer_init(b)) {
        free(b);
        return 0;
    }

    b->event_loop_destroy.notify = handle_event_loop_destroy;
    wl_event_loop_add_destroy_listener(loop, &b->event_loop_destroy);

    return &b->backend;
}

bool wlr_backend_is_native(struct wlr_backend* backend) {
    return backend->impl == &backend_impl;
}
