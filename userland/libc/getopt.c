// ── getopt.c — POSIX argv parser + getopt_long (GNU extension) ──────
//
// Single-threaded only — globals are not TLS.  Matches the glibc
// semantics close enough for dwl, sway, and typical daemon CLIs.

#include <getopt.h>
#include <string.h>
#include <stdio.h>

char* optarg = 0;
int   optind = 1;
int   opterr = 1;
int   optopt = 0;

// State carried across calls within one argv scan.
static int s_nextchar = 0;      // offset within current argv[optind]

static void reset_state(void) {
    s_nextchar = 0;
    optarg = 0;
}

int getopt(int argc, char* const argv[], const char* optstring) {
    if (optind <= 0) optind = 1;
    if (s_nextchar == 0) {
        // Advance to the next arg that looks like an option.
        while (optind < argc) {
            const char* a = argv[optind];
            if (a[0] == '-' && a[1] == '-' && a[2] == 0) {
                // "--" terminator
                optind++;
                return -1;
            }
            if (a[0] == '-' && a[1] != 0) { s_nextchar = 1; break; }
            // Not an option — POSIX-default mode stops scanning here.
            return -1;
        }
        if (optind >= argc) return -1;
    }

    const char* arg = argv[optind];
    int c = arg[s_nextchar++];
    const char* hit = strchr(optstring, c);
    if (!hit || c == ':') {
        optopt = c;
        if (opterr && optstring[0] != ':')
            fprintf(stderr, "unknown option '-%c'\n", c);
        if (arg[s_nextchar] == 0) { optind++; reset_state(); }
        return '?';
    }
    if (hit[1] == ':') {
        // Option requires an argument.
        if (arg[s_nextchar] != 0) {
            optarg = (char*)&arg[s_nextchar];
            optind++; reset_state();
        } else if (optind + 1 < argc) {
            optarg = (char*)argv[optind + 1];
            optind += 2; reset_state();
        } else {
            optopt = c;
            if (opterr) fprintf(stderr, "option '-%c' requires an argument\n", c);
            return optstring[0] == ':' ? ':' : '?';
        }
    } else {
        if (arg[s_nextchar] == 0) { optind++; reset_state(); }
    }
    return c;
}

int getopt_long(int argc, char* const argv[],
                const char* shortopts,
                const struct option* longopts, int* longind) {
    if (optind >= argc) return -1;
    const char* a = argv[optind];
    if (a[0] == '-' && a[1] == '-' && a[2] != 0) {
        const char* name  = a + 2;
        const char* eq    = strchr(name, '=');
        size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
        for (int i = 0; longopts[i].name; i++) {
            if (strncmp(longopts[i].name, name, nlen) == 0 && longopts[i].name[nlen] == 0) {
                if (longind) *longind = i;
                optind++;
                if (longopts[i].has_arg == required_argument) {
                    if (eq) optarg = (char*)(eq + 1);
                    else if (optind < argc) optarg = (char*)argv[optind++];
                    else {
                        if (opterr) fprintf(stderr, "option '--%s' requires an argument\n", longopts[i].name);
                        return '?';
                    }
                } else if (longopts[i].has_arg == optional_argument) {
                    optarg = eq ? (char*)(eq + 1) : 0;
                } else {
                    optarg = 0;
                }
                if (longopts[i].flag) { *longopts[i].flag = longopts[i].val; return 0; }
                return longopts[i].val;
            }
        }
        optind++;
        if (opterr) fprintf(stderr, "unknown option '--%.*s'\n", (int)nlen, name);
        return '?';
    }
    return getopt(argc, argv, shortopts);
}
