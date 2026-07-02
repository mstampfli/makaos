#pragma once
#include "common.h"   // NULL

// ── Intrusive singly-linked FIFO ────────────────────────────────────────────
//
// One correct mechanism for the enqueue-at-tail / dequeue-from-head queue that
// was hand-rolled identically across the udp_rx, unix-datagram, unix-accept and
// io_uring-overflow queues.  The subtle invariant -- reset `tail` to NULL when
// the last element is dequeued, or a stale tail dangles and the next enqueue
// writes through freed memory -- lives here ONCE so it cannot be got wrong or
// drift between call sites.
//
// The node type must have a `next` field of its own pointer type.  `head`,
// `tail`, `count` and `node`/`out` are plain lvalues (a struct member or local);
// they are evaluated more than once, so pass side-effect-free lvalues only.
// These touch ONLY the head/tail/count -- the caller owns any lock and any
// full-queue check (do the check before FIFO_ENQUEUE_TAIL) and frees the node
// it pops.

// Append `node` at the tail (its next is nulled) and bump `count`.
#define FIFO_ENQUEUE_TAIL(head, tail, count, node) do { \
        (node)->next = NULL;                            \
        if (tail) (tail)->next = (node);                \
        else      (head)       = (node);                \
        (tail)    = (node);                             \
        (count)++;                                      \
} while (0)

// Pop the head into `out` (set to NULL if the queue was empty).  On a non-empty
// queue: unlink the head, reset `tail` to NULL if the queue is now empty, and
// drop `count`.  Unlinks only -- the caller frees `out`.
#define FIFO_DEQUEUE_HEAD(head, tail, count, out) do {  \
        (out) = (head);                                 \
        if (out) {                                      \
                (head) = (out)->next;                   \
                if (!(head)) (tail) = NULL;             \
                (count)--;                              \
        }                                               \
} while (0)
