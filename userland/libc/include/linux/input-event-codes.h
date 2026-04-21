#ifndef _MAKAOS_LINUX_INPUT_EVENT_CODES_H
#define _MAKAOS_LINUX_INPUT_EVENT_CODES_H 1

// Porting-layer shim — pure constants describing the Linux evdev
// protocol.  MakaOS uses Linux-compatible KEY_* / BTN_* / REL_* /
// ABS_* numbering in <makaos/input.h>; this header just re-exposes
// them under the path Linux ports expect.  No runtime code, no
// struct layout — protocol constants only.

#include <makaos/input.h>

#endif
