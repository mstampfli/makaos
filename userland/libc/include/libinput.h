#ifndef _MAKAOS_LIBINPUT_H
#define _MAKAOS_LIBINPUT_H 1

// Porting-layer shim — enum types ONLY, no runtime bindings.
// dwl's config.h declares variables of these types that would be
// passed to libinput_device_config_*() calls if the wlroots libinput
// backend were active.  MakaOS uses a native wlroots backend instead,
// so those calls are patched out and these values are unreachable at
// runtime.  The shim exists purely so the variable declarations in
// config.h compile.

enum libinput_config_scroll_method {
    LIBINPUT_CONFIG_SCROLL_NO_SCROLL      = 0,
    LIBINPUT_CONFIG_SCROLL_2FG            = (1 << 0),
    LIBINPUT_CONFIG_SCROLL_EDGE           = (1 << 1),
    LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN = (1 << 2),
};

enum libinput_config_click_method {
    LIBINPUT_CONFIG_CLICK_METHOD_NONE         = 0,
    LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS = (1 << 0),
    LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER  = (1 << 1),
};

enum libinput_config_send_events_mode {
    LIBINPUT_CONFIG_SEND_EVENTS_ENABLED                  = 0,
    LIBINPUT_CONFIG_SEND_EVENTS_DISABLED                 = (1 << 0),
    LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE = (1 << 1),
};

enum libinput_config_accel_profile {
    LIBINPUT_CONFIG_ACCEL_PROFILE_NONE     = 0,
    LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT     = (1 << 0),
    LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE = (1 << 1),
};

enum libinput_config_tap_button_map {
    LIBINPUT_CONFIG_TAP_MAP_LRM = 0,
    LIBINPUT_CONFIG_TAP_MAP_LMR,
};

enum libinput_config_tap_state {
    LIBINPUT_CONFIG_TAP_DISABLED = 0,
    LIBINPUT_CONFIG_TAP_ENABLED,
};

enum libinput_config_drag_state {
    LIBINPUT_CONFIG_DRAG_DISABLED = 0,
    LIBINPUT_CONFIG_DRAG_ENABLED,
};

enum libinput_config_drag_lock_state {
    LIBINPUT_CONFIG_DRAG_LOCK_DISABLED = 0,
    LIBINPUT_CONFIG_DRAG_LOCK_ENABLED,
};

#endif
