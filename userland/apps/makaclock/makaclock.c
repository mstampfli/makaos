// makaclock — swaybar status helper (a clock).
//
// swaybar runs a `status_command` and shows each line it prints.  A shell
// clock loop can't work on this image: the bash build has no `printf '%()T'`
// and no `read -t` timeout, and it block-buffers builtin output to a pipe, so
// the loop either exits (swaybar then renders the red "[error reading from
// status command]") or busy-fork-storms a subshell-per-tick.  This tiny native
// helper avoids all of that: it formats the time itself, writes it with a
// single direct (unbuffered) write(2), sleeps one second via nanosleep, and
// never exits — so swaybar's text protocol updates cleanly once per second.
//
// MakaOS has no timezone database, so the time is reported as UTC.

#include "libc.h"

int main(void) {
    char buf[64];
    for (;;) {
        time_t t = time((time_t*)0);
        struct tm tm;
        gmtime_r(&t, &tm);   // UTC — no tz db on MakaOS
        int n = snprintf(buf, sizeof buf,
                         "MakaOS   %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (n > 0) {
            if (n > (int)sizeof buf) n = (int)sizeof buf;
            write(1, buf, (size_t)n);   // direct write: no stdio buffering
        }
        sleep(1);
    }
    return 0;
}
