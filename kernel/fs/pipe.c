#include "pipe.h"
#include "kheap.h"
#include "sched.h"
#include "errno.h"

// ── Pipe VFS callbacks ────────────────────────────────────────────────────

static int64_t pipe_read(vfs_file_t* self, void* buf, uint64_t len) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;

    while (total < len) {
        // Wait for data.
        while (p->count == 0) {
            if (p->writer_refs == 0) return (int64_t)total; // EOF
            sched_sleep();
        }
        dst[total++] = p->buf[p->head];
        p->head = (p->head + 1) & (PIPE_BUF_SIZE - 1);
        p->count--;
    }
    return (int64_t)total;
}

static int64_t pipe_write(vfs_file_t* self, const void* buf, uint64_t len) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    const uint8_t* src = (const uint8_t*)buf;
    uint64_t total = 0;

    if (p->reader_refs == 0) return (int64_t)-EPIPE;

    while (total < len) {
        // Wait for space.
        while (p->count == PIPE_BUF_SIZE) {
            if (p->reader_refs == 0) return total ? (int64_t)total : (int64_t)-EPIPE;
            sched_sleep();
        }
        p->buf[p->tail] = src[total++];
        p->tail = (p->tail + 1) & (PIPE_BUF_SIZE - 1);
        p->count++;
    }

    // Wake reader polling on this pipe.
    if (p->read_file && p->read_file->poll_waiter) {
        task_t* w = (task_t*)p->read_file->poll_waiter;
        p->read_file->poll_waiter = NULL;
        sched_wake(w);
    }

    return (int64_t)total;
}

static void pipe_read_close(vfs_file_t* self) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    if (p->reader_refs > 0) p->reader_refs--;
    if (p->reader_refs == 0 && p->writer_refs == 0) kfree(p);
    kfree(self);
}

static void pipe_write_close(vfs_file_t* self) {
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    if (p->writer_refs > 0) p->writer_refs--;
    if (p->reader_refs == 0 && p->writer_refs == 0) kfree(p);
    kfree(self);
}

// poll: check readiness without blocking.
static int pipe_read_poll(vfs_file_t* self, int events) {
    (void)events;
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    // Readable if data available OR write end closed (EOF).
    return (p->count > 0 || p->writer_refs == 0) ? 1 : 0;
}

static int pipe_write_poll(vfs_file_t* self, int events) {
    (void)events;
    pipe_buf_t* p = (pipe_buf_t*)self->ctx;
    // Writable if space available and read end still open.
    return (p->count < PIPE_BUF_SIZE && p->reader_refs > 0) ? 1 : 0;
}

// ── pipe_create ───────────────────────────────────────────────────────────

int pipe_create(vfs_file_t** read_end, vfs_file_t** write_end) {
    pipe_buf_t* p = kmalloc(sizeof(pipe_buf_t));
    if (!p) return -ENOMEM;

    for (int i = 0; i < PIPE_BUF_SIZE; i++) p->buf[i] = 0;
    p->head = p->tail = p->count = 0;
    p->writer_refs = 1;
    p->reader_refs = 1;

    vfs_file_t* r = kmalloc(sizeof(vfs_file_t));
    vfs_file_t* w = kmalloc(sizeof(vfs_file_t));
    if (!r || !w) {
        if (r) kfree(r);
        if (w) kfree(w);
        kfree(p);
        return -ENOMEM;
    }

    r->read        = pipe_read;
    r->write       = NULL;
    r->seek        = NULL;
    r->close       = pipe_read_close;
    r->poll        = pipe_read_poll;
    r->ioctl       = NULL;
    r->ctx         = p;
    r->poll_waiter = NULL;
    r->flags       = 0;
    r->refcount    = 1;
    r->rights      = 0;
    r->path[0]     = '\0';

    w->read        = NULL;
    w->write       = pipe_write;
    w->seek        = NULL;
    w->close       = pipe_write_close;
    w->poll        = pipe_write_poll;
    w->ioctl       = NULL;
    w->ctx         = p;
    w->poll_waiter = NULL;
    w->flags       = 0;
    w->refcount    = 1;
    w->rights      = 0;
    w->path[0]     = '\0';

    p->read_file  = r;
    p->write_file = w;

    *read_end  = r;
    *write_end = w;
    return 0;
}
