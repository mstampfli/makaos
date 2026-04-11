#include "libc.h"
#include "stdio.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: mkdir <dir> [...]\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            fprintf(stderr, "mkdir: cannot create '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
