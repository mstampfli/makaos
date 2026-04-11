#include "libc.h"
#include "stdio.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: mv <src> <dst>\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        fprintf(stderr, "mv: failed to rename '%s' to '%s'\n", argv[1], argv[2]);
        return 1;
    }
    return 0;
}
