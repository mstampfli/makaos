#include "libc.h"

int main(void) {
    write(1, "Rebooting...\n", 13);
    syscall0(SYS_REBOOT);
    for (;;);
    return 0;
}
