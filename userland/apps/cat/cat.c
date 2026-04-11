#include "libc.h"
#include "stdio.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: cat <file> [...]\n");
        return 1;
    }

    char buf[4096];
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (size_t)n);
        close(fd);
    }
    return 0;
}
