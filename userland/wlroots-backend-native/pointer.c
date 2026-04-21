// MakaOS native wlroots backend — pointer translation.
//
// REL_X / REL_Y produce wlr_pointer_motion_event (relative).
// BTN_LEFT / BTN_RIGHT / BTN_MIDDLE produce wlr_pointer_button_event.
// REL_WHEEL produces wlr_pointer_axis_event.
//
// All events carry a wlroots msec timestamp derived from the kernel's
// ns counter.

#include <wlr/interfaces/wlr_pointer.h>
#include <makaos/input.h>
#include "native.h"

static const struct wlr_pointer_impl native_pointer_impl = {
    .name = "makaos-pointer",
};

bool native_pointer_init(struct wlr_native_backend* b) {
    wlr_pointer_init(&b->pointer, &native_pointer_impl, "makaos-pointer");
    return true;
}

void native_dispatch_button(struct wlr_native_backend* b, uint16_t code,
                            int32_t value, uint64_t time_ns) {
    struct wlr_pointer_button_event ev = {
        .pointer   = &b->pointer,
        .time_msec = (uint32_t)(time_ns / 1000000ull),
        .button    = (uint32_t)code,
        .state     = value ? WL_POINTER_BUTTON_STATE_PRESSED
                           : WL_POINTER_BUTTON_STATE_RELEASED,
    };
    wl_signal_emit_mutable(&b->pointer.events.button, &ev);
    // Pointer events frame after each button transition.
    wl_signal_emit_mutable(&b->pointer.events.frame, &b->pointer);
}

void native_dispatch_rel(struct wlr_native_backend* b, uint16_t code,
                         int32_t value, uint64_t time_ns) {
    uint32_t time_msec = (uint32_t)(time_ns / 1000000ull);
    if (code == REL_X || code == REL_Y) {
        struct wlr_pointer_motion_event ev = {
            .pointer   = &b->pointer,
            .time_msec = time_msec,
            .delta_x   = (code == REL_X) ? (double)value : 0.0,
            .delta_y   = (code == REL_Y) ? (double)value : 0.0,
            .unaccel_dx= (code == REL_X) ? (double)value : 0.0,
            .unaccel_dy= (code == REL_Y) ? (double)value : 0.0,
        };
        wl_signal_emit_mutable(&b->pointer.events.motion, &ev);
        wl_signal_emit_mutable(&b->pointer.events.frame,  &b->pointer);
    } else if (code == REL_WHEEL) {
        struct wlr_pointer_axis_event ev = {
            .pointer        = &b->pointer,
            .time_msec      = time_msec,
            .source         = WL_POINTER_AXIS_SOURCE_WHEEL,
            .orientation    = WL_POINTER_AXIS_VERTICAL_SCROLL,
            .delta          = (double)(-value) * 15.0,     // 15 px per click
            .delta_discrete = -value,
        };
        wl_signal_emit_mutable(&b->pointer.events.axis,  &ev);
        wl_signal_emit_mutable(&b->pointer.events.frame, &b->pointer);
    }
}
