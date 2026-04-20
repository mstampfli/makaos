#ifndef _MAKAOS_IO_URING_H
#define _MAKAOS_IO_URING_H 1
// MakaOS io_uring — Linux-compatible layouts for direct drop-in.
//
// Structures MUST stay ABI-identical to kernel/io/io_uring.h.
// Static asserts on the kernel side validate sizeof() in CI.

#include <stdint.h>
#include <sys/types.h>

typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t ioprio;
    int32_t  fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t rw_flags;
    uint64_t user_data;
    uint16_t buf_index;
    uint16_t personality;
    int32_t  splice_fd_in;
    uint64_t __pad2[2];
} io_uring_sqe;

typedef struct {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
} io_uring_cqe;

typedef struct {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct {
        uint32_t head, tail, ring_mask, ring_entries;
        uint32_t flags, dropped;
        uint32_t array, resv1;
        uint64_t resv2;
    } sq_off;
    struct {
        uint32_t head, tail, ring_mask, ring_entries;
        uint32_t overflow, cqes, flags, resv1;
        uint64_t resv2;
    } cq_off;
} io_uring_params;

// SQE opcodes
#define IORING_OP_NOP      0
#define IORING_OP_READV    1
#define IORING_OP_WRITEV   2
#define IORING_OP_READ     22
#define IORING_OP_WRITE    23
#define IORING_OP_ACCEPT   13
#define IORING_OP_CONNECT  16
#define IORING_OP_SEND     26
#define IORING_OP_RECV     27
#define IORING_OP_CLOSE    19

// SQE flags
#define IOSQE_FIXED_FILE   0x01
#define IOSQE_IO_DRAIN     0x02
#define IOSQE_IO_LINK      0x04
#define IOSQE_ASYNC        0x10

// Setup flags
#define IORING_SETUP_IOPOLL 0x01
#define IORING_SETUP_SQPOLL 0x02

// enter flags
#define IORING_ENTER_GETEVENTS 0x01
#define IORING_ENTER_SQ_WAKEUP 0x02

int io_uring_setup(unsigned entries, io_uring_params* p);
int io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
                    unsigned flags, const void* sig);
int io_uring_register(int ring_fd, unsigned op, const void* arg, unsigned n);

#endif
