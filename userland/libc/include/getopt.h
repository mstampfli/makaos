#ifndef _MAKAOS_GETOPT_H
#define _MAKAOS_GETOPT_H 1

// POSIX getopt — global state + basic long-option struct.
// libc.c provides the runtime impl.

extern char* optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;

int getopt(int argc, char* const argv[], const char* optstring);

struct option {
    const char* name;
    int         has_arg;   // 0=no_argument, 1=required_argument, 2=optional_argument
    int*        flag;
    int         val;
};

#define no_argument       0
#define required_argument 1
#define optional_argument 2

int getopt_long(int argc, char* const argv[],
                const char* shortopts,
                const struct option* longopts, int* longind);

#endif
