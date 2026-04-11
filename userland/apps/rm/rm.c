#include "libc.h"
#include "stdio.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: rm <file> [...]\n");
        return 1;
    }
    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) < 0) {
            fprintf(stderr, "rm: cannot remove '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
