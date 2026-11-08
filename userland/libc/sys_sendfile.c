// sys_sendfile.c - <sys/sendfile.h>.
//
// MakaOS has no sendfile syscall, so copy in_fd -> out_fd through a bounce
// buffer. Matches the Linux semantics: a non-NULL offset uses pread and
// advances *offset without moving in_fd's file position; a NULL offset uses
// and advances in_fd's own position.

#include <sys/sendfile.h>
#include <unistd.h>

ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
    char buf[65536];
    ssize_t total = 0;
    off_t off = offset ? *offset : 0;
    while (count > 0) {
        size_t chunk = count < sizeof(buf) ? count : sizeof(buf);
        ssize_t r = offset ? pread(in_fd, buf, chunk, off)
                           : read(in_fd, buf, chunk);
        if (r < 0)  return total > 0 ? total : -1;
        if (r == 0) break;
        ssize_t w = 0;
        while (w < r) {
            ssize_t x = write(out_fd, buf + w, (size_t)(r - w));
            if (x < 0) return total > 0 ? total : -1;
            w += x;
        }
        total += r; off += r; count -= (size_t)r;
    }
    if (offset) *offset = off;
    return total;
}

ssize_t sendfile64(int out_fd, int in_fd, off64_t* offset, size_t count) {
    return sendfile(out_fd, in_fd, (off_t*)offset, count);
}
