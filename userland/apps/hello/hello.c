#include "libc.h"

int main(void) {
    printf("Hello from user space!\n");
    printf("PID: %d\n", getpid());

    // List /bin.
    dirent_t entries[16];
    int n = readdir("/bin", 4, entries, 16);
    if (n > 0) {
        printf("/bin (%d entries):\n", n);
        for (int i = 0; i < n; i++) {
            printf("  %s\n", entries[i].name);
        }
    }

    printf("bye!\n");
    exit(0);
}
