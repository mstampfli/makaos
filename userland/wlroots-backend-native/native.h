// Internal header for the MakaOS native wlroots backend.
// Public API: wlr/backend/native.h (wlr_native_backend_create).

#ifndef BACKEND_NATIVE_H
#define BACKEND_NATIVE_H

#include <wayland-server-core.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/native.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

struct wlr_native_backend {
    struct wlr_backend     backend;
    struct wl_event_loop*  event_loop;
    bool                   started;

    // Input devices.  One keyboard + one pointer for now; matches our
    // single /dev/input/event0 kernel node.  Grows as the device
    // registry lands (scalability-debt-ledger-#3).
    struct wlr_keyboard    keyboard;
    struct wlr_pointer     pointer;

    // /dev/input/event0 fd + its wl_event_source in the loop.
    int                    input_fd;
    struct wl_event_source* input_source;

    // Listener for event-loop destruction so we tear down cleanly.
    struct wl_listener     event_loop_destroy;
};

// Entry points shared between backend.c / keyboard.c / pointer.c.
bool native_keyboard_init(struct wlr_native_backend* b);
bool native_pointer_init (struct wlr_native_backend* b);

// Event dispatch — called from backend.c's fd-readable handler with
// a single decoded MakaOS event.
void native_dispatch_key    (struct wlr_native_backend* b, uint16_t code, int32_t value, uint64_t time_ns);
void native_dispatch_button (struct wlr_native_backend* b, uint16_t code, int32_t value, uint64_t time_ns);
void native_dispatch_rel    (struct wlr_native_backend* b, uint16_t code, int32_t value, uint64_t time_ns);

#endif
