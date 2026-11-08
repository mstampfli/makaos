// sys_prctl.c - <sys/prctl.h> (Linux-compatible subset).
//
// MakaOS has no general process-control multiplexer. We accept the naming
// options as no-ops (they are diagnostic-only) and report EINVAL for the rest
// so callers treat the feature as unavailable and continue. OpenJDK, for
// example, only probes PR_*_TIMERSLACK and tolerates failure.

#include <sys/prctl.h>
#include <stdarg.h>
#include <errno.h>

int prctl(int option, ...) {
    switch (option) {
    case PR_SET_NAME:
    case PR_GET_NAME:
        // No per-thread kernel name facility yet; succeed silently.
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}
