#ifndef _MAKAOS_SYS_UN_H
#define _MAKAOS_SYS_UN_H 1

#include <sys/socket.h>

// AF_UNIX socket address.  Wayland compositors bind to
// $XDG_RUNTIME_DIR/wayland-N, clients connect to it.

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 108
#endif

struct sockaddr_un {
    uint16_t sun_family;                    // AF_UNIX
    char     sun_path[UNIX_PATH_MAX];       // NUL-terminated path
};

#endif
