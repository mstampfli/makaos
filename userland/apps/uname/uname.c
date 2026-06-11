// ── uname — system identification ────────────────────────────────────
// sway logs `uname -a` via popen at startup; bash scripts probe -s/-m.
// Values are fixed at build time — MakaOS has one kernel and one arch.

#include "libc.h"

#define SYSNAME  "MakaOS"
#define NODENAME "makaos"
#define RELEASE  "0.9.0"
#define VERSION  "test"
#define MACHINE  "x86_64"

static void put(const char* s, int* first) {
    if (!*first) write(1, " ", 1);
    write(1, s, strlen(s));
    *first = 0;
}

int main(int argc, char** argv) {
    int s = 0, n = 0, r = 0, v = 0, m = 0;

    if (argc == 1) {
        s = 1;
    } else {
        for (int i = 1; i < argc; i++) {
            const char* a = argv[i];
            if (a[0] != '-') continue;
            for (int j = 1; a[j]; j++) {
                switch (a[j]) {
                case 'a': s = n = r = v = m = 1; break;
                case 's': s = 1; break;
                case 'n': n = 1; break;
                case 'r': r = 1; break;
                case 'v': v = 1; break;
                case 'm': case 'p': m = 1; break;
                case 'o': s = 1; break;
                default:
                    write(2, "uname: unknown option\n", 22);
                    return 1;
                }
            }
        }
    }

    int first = 1;
    if (s) put(SYSNAME,  &first);
    if (n) put(NODENAME, &first);
    if (r) put(RELEASE,  &first);
    if (v) put(VERSION,  &first);
    if (m) put(MACHINE,  &first);
    write(1, "\n", 1);
    return 0;
}
