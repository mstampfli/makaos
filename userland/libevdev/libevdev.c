// MakaOS minimal libevdev.  Lookup tables for the KEY_* and BTN_*
// names sway's config parser accepts.  Linear scan (~120 entries);
// called once per config-line binding, not a hot path.
//
// TODO(scalability-debt-ledger-#3): when kernel grows a device-caps
// query ioctl, swap this static table for a dynamic capability scan.

#include "libevdev.h"
#include <makaos/input.h>
#include <string.h>

struct name_entry { const char* name; unsigned int code; };

// Keep alphabetized within groups; linear scan on ~120 items is fine
// for config-parse, which happens once at compositor start.
static const struct name_entry s_key_names[] = {
    // Alphanumerics
    {"KEY_0", KEY_0}, {"KEY_1", KEY_1}, {"KEY_2", KEY_2}, {"KEY_3", KEY_3},
    {"KEY_4", KEY_4}, {"KEY_5", KEY_5}, {"KEY_6", KEY_6}, {"KEY_7", KEY_7},
    {"KEY_8", KEY_8}, {"KEY_9", KEY_9},
    {"KEY_A", KEY_A}, {"KEY_B", KEY_B}, {"KEY_C", KEY_C}, {"KEY_D", KEY_D},
    {"KEY_E", KEY_E}, {"KEY_F", KEY_F}, {"KEY_G", KEY_G}, {"KEY_H", KEY_H},
    {"KEY_I", KEY_I}, {"KEY_J", KEY_J}, {"KEY_K", KEY_K}, {"KEY_L", KEY_L},
    {"KEY_M", KEY_M}, {"KEY_N", KEY_N}, {"KEY_O", KEY_O}, {"KEY_P", KEY_P},
    {"KEY_Q", KEY_Q}, {"KEY_R", KEY_R}, {"KEY_S", KEY_S}, {"KEY_T", KEY_T},
    {"KEY_U", KEY_U}, {"KEY_V", KEY_V}, {"KEY_W", KEY_W}, {"KEY_X", KEY_X},
    {"KEY_Y", KEY_Y}, {"KEY_Z", KEY_Z},
    // Function keys
    {"KEY_F1", KEY_F1}, {"KEY_F2", KEY_F2}, {"KEY_F3", KEY_F3}, {"KEY_F4", KEY_F4},
    {"KEY_F5", KEY_F5}, {"KEY_F6", KEY_F6}, {"KEY_F7", KEY_F7}, {"KEY_F8", KEY_F8},
    {"KEY_F9", KEY_F9}, {"KEY_F10", KEY_F10},{"KEY_F11", KEY_F11},{"KEY_F12", KEY_F12},
    // Navigation
    {"KEY_ESC", KEY_ESC}, {"KEY_TAB", KEY_TAB},
    {"KEY_ENTER", KEY_ENTER}, {"KEY_SPACE", KEY_SPACE},
    {"KEY_BACKSPACE", KEY_BACKSPACE},
    {"KEY_HOME", KEY_HOME}, {"KEY_END", KEY_END},
    {"KEY_PAGEUP", KEY_PAGEUP}, {"KEY_PAGEDOWN", KEY_PAGEDOWN},
    {"KEY_INSERT", KEY_INSERT}, {"KEY_DELETE", KEY_DELETE},
    {"KEY_UP", KEY_UP}, {"KEY_DOWN", KEY_DOWN},
    {"KEY_LEFT", KEY_LEFT}, {"KEY_RIGHT", KEY_RIGHT},
    // Punctuation
    {"KEY_MINUS", KEY_MINUS}, {"KEY_EQUAL", KEY_EQUAL},
    {"KEY_LEFTBRACE", KEY_LEFTBRACE}, {"KEY_RIGHTBRACE", KEY_RIGHTBRACE},
    {"KEY_SEMICOLON", KEY_SEMICOLON}, {"KEY_APOSTROPHE", KEY_APOSTROPHE},
    {"KEY_GRAVE", KEY_GRAVE}, {"KEY_BACKSLASH", KEY_BACKSLASH},
    {"KEY_COMMA", KEY_COMMA}, {"KEY_DOT", KEY_DOT}, {"KEY_SLASH", KEY_SLASH},
    // Modifiers
    {"KEY_LEFTSHIFT", KEY_LEFTSHIFT}, {"KEY_RIGHTSHIFT", KEY_RIGHTSHIFT},
    {"KEY_LEFTCTRL", KEY_LEFTCTRL},   {"KEY_RIGHTCTRL", KEY_RIGHTCTRL},
    {"KEY_LEFTALT", KEY_LEFTALT},     {"KEY_RIGHTALT", KEY_RIGHTALT},
    {"KEY_CAPSLOCK", KEY_CAPSLOCK},
    // Locks
    {"KEY_NUMLOCK", KEY_NUMLOCK}, {"KEY_SCROLLLOCK", KEY_SCROLLLOCK},
    // Keypad
    {"KEY_KP0", KEY_KP0}, {"KEY_KP1", KEY_KP1}, {"KEY_KP2", KEY_KP2},
    {"KEY_KP3", KEY_KP3}, {"KEY_KP4", KEY_KP4}, {"KEY_KP5", KEY_KP5},
    {"KEY_KP6", KEY_KP6}, {"KEY_KP7", KEY_KP7}, {"KEY_KP8", KEY_KP8},
    {"KEY_KP9", KEY_KP9}, {"KEY_KPDOT", KEY_KPDOT},
    {"KEY_KPPLUS", KEY_KPPLUS}, {"KEY_KPMINUS", KEY_KPMINUS},
    {"KEY_KPASTERISK", KEY_KPASTERISK},
    {"KEY_102ND", KEY_102ND},
    // Mouse buttons (evdev puts them in the KEY event-type namespace).
    {"BTN_LEFT",   BTN_LEFT},
    {"BTN_RIGHT",  BTN_RIGHT},
    {"BTN_MIDDLE", BTN_MIDDLE},
    {"BTN_MOUSE",  BTN_MOUSE},
};
static const int s_key_count = (int)(sizeof(s_key_names) / sizeof(s_key_names[0]));

int libevdev_event_code_from_name(unsigned int type, const char* name) {
    if (type != EV_KEY || !name) return -1;
    for (int i = 0; i < s_key_count; i++) {
        if (strcmp(s_key_names[i].name, name) == 0)
            return (int)s_key_names[i].code;
    }
    return -1;
}

const char* libevdev_event_code_get_name(unsigned int type, unsigned int code) {
    if (type != EV_KEY) return 0;
    for (int i = 0; i < s_key_count; i++) {
        if (s_key_names[i].code == code) return s_key_names[i].name;
    }
    return 0;
}
