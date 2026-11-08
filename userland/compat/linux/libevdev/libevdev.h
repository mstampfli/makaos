#ifndef _MAKAOS_LIBEVDEV_H
#define _MAKAOS_LIBEVDEV_H

// Minimum subset of libevdev used by sway's config parser.

// Event types — match Linux evdev numbering so config-string parsing
// interchanges with Linux tools.
#define EV_SYN   0
#define EV_KEY   1
#define EV_REL   2
#define EV_ABS   3

// name → event code.  Returns -1 if unknown.
int libevdev_event_code_from_name(unsigned int type, const char* name);

// event code → name.  Returns NULL if unknown.
const char* libevdev_event_code_get_name(unsigned int type, unsigned int code);

#endif
