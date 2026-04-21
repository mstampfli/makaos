// MakaOS native wlroots backend — keyboard translation.
//
// MakaOS key codes match Linux evdev numbering (kernel's input_core.h
// uses identical KEY_* constants), so we pass the code through to
// wlroots unchanged.  value 1/0/2 maps to pressed/released/repeat.

#include <wlr/interfaces/wlr_keyboard.h>
#include "native.h"

static const struct wlr_keyboard_impl native_keyboard_impl = {
    .name = "makaos-keyboard",
};

bool native_keyboard_init(struct wlr_native_backend* b) {
    wlr_keyboard_init(&b->keyboard, &native_keyboard_impl, "makaos-keyboard");
    return true;
}

void native_dispatch_key(struct wlr_native_backend* b, uint16_t code,
                         int32_t value, uint64_t time_ns) {
    // wlroots expects millisecond timestamps (uint32_t) in wlr_keyboard_key_event.
    // value semantics: 0 = released, 1 = pressed, 2 = autorepeat.
    struct wlr_keyboard_key_event ev = {
        .time_msec   = (uint32_t)(time_ns / 1000000ull),
        .keycode     = (uint32_t)code,
        .state       = value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
        .update_state= true,
    };
    wlr_keyboard_notify_key(&b->keyboard, &ev);
}
