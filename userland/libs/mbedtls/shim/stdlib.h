#pragma once
// Shim forwarding <stdlib.h> → MakaOS userland libc.
// Used when building mbedTLS with -nostdinc.  Keeps mbedTLS away from
// /usr/include (which fights our libc over fd_set, pselect, etc.).
#include "libc.h"
