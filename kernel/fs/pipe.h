#pragma once
#include "common.h"
#include "vfs.h"

// ── Kernel pipe ───────────────────────────────────────────────────────────
// A pipe is a one-directional byte stream backed by a circular kernel buffer.
// pipe(fds[2]) returns fds[0]=read end, fds[1]=write end.
//
// Blocking behaviour:
//   read:  if buffer empty and write end open, sleep until data arrives.
//          if write end closed, returns 0 (EOF).
//   write: if buffer full, sleep until space is available.
//          if read end closed, returns -EPIPE (SIGPIPE is delivered).
//
// Blocked readers and writers register task_we_t nodes on the relevant
// vfs_file_t->waitq and are woken via wait_queue_wake_all.  No single-
// pointer waiter field — SMP-safe by construction.

#define PIPE_BUF_SIZE 4096  // must be power of 2

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;          // read index
    uint32_t tail;          // write index
    uint32_t count;         // bytes currently in buffer
    uint8_t  writer_refs;   // number of open write-end fds
    uint8_t  reader_refs;   // number of open read-end fds
    struct vfs_file_t* read_file;   // back-pointer — its waitq handles
                                    // blocking readers + poll/epoll
    struct vfs_file_t* write_file;  // back-pointer — its waitq handles
                                    // blocking writers + poll/epoll
} pipe_buf_t;

// Create a pipe; fills fds[0] (read) and fds[1] (write).
// Returns 0 on success, -errno on failure.
int pipe_create(vfs_file_t** read_end, vfs_file_t** write_end);
