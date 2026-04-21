#ifndef _MAKAOS_SYS_IOCTL_H
#define _MAKAOS_SYS_IOCTL_H 1

// Linux _IOC / _IOR / _IOW / _IOWR live in <asm/ioctl.h> — expose
// them via sys/ioctl.h too so libinput / libevdev / any Linux-ABI
// port's input-event-codes.h lookups resolve without per-library fixups.
#include <asm/ioctl.h>

// TTY window size
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCNOTTY  0x5422
#define TIOCSCTTY  0x540E

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long req, ...);

#endif
