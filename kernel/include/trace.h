#pragma once
#include "common.h"

/* ── Tracepoints + event ring — DEBUGGING.md §4 ───────────────────────
 *
 * Tracepoints are named hook points emitting structured events into
 * a per-CPU lock-free ring (§4.2).  The ring is:
 *
 *   - Per-CPU.  No cross-CPU cache-line ping-pong on hot paths.
 *   - Fixed-size entries.  No allocation on the hot path.
 *   - Lock-free circular.  A head index moves forward mod capacity;
 *     overflow silently overwrites the oldest event.
 *   - Dumped on panic (trace_ring_dump) to reconstruct the last few
 *     milliseconds of activity when the log alone is insufficient.
 *
 * Event schema: every event carries a timestamp (tsc_read_ns), a
 * tag (category string pointer — lives in .rodata, hence comparable
 * by pointer), and up to 4 uint64_t args.  Keep the arg vocabulary
 * subsystem-specific: (syscall_nr, arg0, arg1, retval), (prev_pid,
 * next_pid, prev_prio, next_prio), etc.
 *
 * ── Usage ─────────────────────────────────────────────────────────
 *   TRACE(TRACE_DRM_IOCTL, req, arg, result, fb_id);
 *
 * Expanding trace sites:
 *   - Add the tag to the TRACE_TAGS list below (.rodata string).
 *   - Include this header and call TRACE(tag, ...).
 *
 * Compile-time gate: CONFIG_TRACE (default on in debug builds).
 */

#ifndef CONFIG_TRACE
#define CONFIG_TRACE 1
#endif

/* One ring entry. */
typedef struct {
    uint64_t    ns;         /* monotonic ns via tsc_read_ns */
    const char* tag;        /* pointer into .rodata — no lifetime issues */
    uint64_t    a0, a1, a2, a3;
    uint32_t    cpu;
    uint32_t    seq;        /* monotonic — 0 in an empty slot */
} trace_event_t;

#define TRACE_RING_CAPACITY  512  /* per CPU */

/* Append an event to the current CPU's ring.  Lock-free; safe from
 * any context including IRQ and panic (panic dumps the ring but
 * does not emit new events from itself). */
void trace_emit(const char* tag,
                 uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

/* Dump every CPU's ring to serial, oldest-first.  Called from panic
 * after the klog ring dump. */
void trace_ring_dump(void);

#if CONFIG_TRACE
#define TRACE(tag, a, b, c, d) \
    trace_emit((tag), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d))
#else
#define TRACE(tag, a, b, c, d) do { (void)(tag); } while (0)
#endif

/* ── Tag strings ──────────────────────────────────────────────────
 *
 * Tags are plain .rodata strings — cheap to compare, cheap to
 * serialize.  Add new tags here as subsystems adopt tracing.
 */

extern const char* const TRACE_SCHED_SWITCH;

extern const char* const TRACE_SYSCALL_ENTER;
extern const char* const TRACE_SYSCALL_EXIT;

extern const char* const TRACE_IPC_SEND;
extern const char* const TRACE_IPC_RECV;
extern const char* const TRACE_IPC_REPLY;

extern const char* const TRACE_VM_FAULT;
extern const char* const TRACE_VM_MAP;
extern const char* const TRACE_VM_UNMAP;

extern const char* const TRACE_FS_READ;
extern const char* const TRACE_FS_WRITE;
extern const char* const TRACE_FS_OPEN;
extern const char* const TRACE_FS_CLOSE;

extern const char* const TRACE_DRM_IOCTL;
extern const char* const TRACE_DRM_COMMIT;
extern const char* const TRACE_DRM_ADDFB;
extern const char* const TRACE_DRM_RMFB;

extern const char* const TRACE_GPU_SET_SCANOUT;
extern const char* const TRACE_GPU_RES_FLUSH;
extern const char* const TRACE_GPU_RES_TRANSFER;

extern const char* const TRACE_SIGNAL_DELIVER;
