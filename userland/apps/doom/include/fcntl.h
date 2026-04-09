#pragma once
// fcntl.h shim — provide O_* flags used by doom
#include "../../../libc/libc.h"

// O_* flags are already in libc.h; add O_NONBLOCK and friends.
#define O_NONBLOCK 0x800
#define O_NOCTTY   0x100
#define O_CLOEXEC  0x80000

// Doom uses open() which is already in libc.h.
