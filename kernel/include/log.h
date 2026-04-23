#pragma once
#include "common.h"

/* ── Structured kernel logging — DEBUGGING.md §2 ───────────────────────
 *
 * Every log line carries:
 *   [    12.345678] [CPU0] [subsys:LEVEL] body
 *
 *   - timestamp : seconds.microseconds since tsc_init, fixed width
 *                 (6-digit-seconds + 6-digit-us) so misaligned
 *                 timestamps never make columns unreadable.
 *   - cpu id    : CPU0..CPUN, non-optional on SMP (per spec §2.1).
 *   - subsys    : short tag (sched, drm, vm, ipc, pipe, svcmgr, ...).
 *                 Matches the function prefix convention.
 *   - level     : DEBUG / INFO / WARN / ERR / CRIT.
 *
 * Emission is atomic per line (one call acquires the serial lock for
 * the whole prefix + body).  An in-memory ring keeps the last
 * KLOG_RING_ENTRIES lines for panic dump — §3.2 item 6.
 *
 * DEBUG is compile-time gated by CONFIG_DEBUG (global) and per-
 * subsystem by CONFIG_DEBUG_<SUBSYS> macros.  Zero cost in release
 * builds when both are 0.  INFO and above are always compiled in.
 *
 * ── Usage ─────────────────────────────────────────────────────────
 *   pr_info("drm", "commit crtc=%u fb=%u", crtc, fb);
 *   pr_warn("net", "arp timeout for %u.%u.%u.%u", a, b, c, d);
 *   pr_err("ext2", "bad magic 0x%x on dev %u", m, dev);
 *   pr_crit("sched", "runqueue corrupted: head=%p",  (void*)head);
 *   pr_debug("drm", "ioctl dispatch=%x arg=%lx", req, arg);
 */

typedef enum log_level {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERR   = 3,
    LOG_CRIT  = 4,
} log_level_t;

/* Emit a formatted log line.  Serial-locked; re-entrant; safe from
 * IRQ context.  NOT safe to call from a panic path that already holds
 * the serial lock — panic code uses klog_panic_emit() instead. */
__attribute__((format(printf, 3, 4)))
void klog_emit(log_level_t level, const char* subsys, const char* fmt, ...);

/* Panic-path emission.  Bypasses the serial lock (caller must own all
 * CPUs or the caller must be the last one standing).  Still appends
 * to the ring so subsequent reads of the ring see the panic line. */
__attribute__((format(printf, 3, 4)))
void klog_panic_emit(log_level_t level, const char* subsys, const char* fmt, ...);

/* Dump the full in-memory ring to serial, oldest-first.  Called from
 * the panic path (§3.2) to surface the last operational context. */
void klog_ring_dump(void);

/* ── Level macros ─────────────────────────────────────────────────── */

#ifndef CONFIG_DEBUG
/* Master debug gate.  Debug builds leave this at 1; release builds
 * compile it out.  Per-subsystem gates refine further. */
#define CONFIG_DEBUG 1
#endif

#if CONFIG_DEBUG
#define pr_debug(subsys, fmt, ...) klog_emit(LOG_DEBUG, (subsys), (fmt), ##__VA_ARGS__)
#else
#define pr_debug(subsys, fmt, ...) do { (void)(subsys); } while (0)
#endif

#define pr_info(subsys, fmt, ...)  klog_emit(LOG_INFO,  (subsys), (fmt), ##__VA_ARGS__)
#define pr_warn(subsys, fmt, ...)  klog_emit(LOG_WARN,  (subsys), (fmt), ##__VA_ARGS__)
#define pr_err(subsys, fmt, ...)   klog_emit(LOG_ERR,   (subsys), (fmt), ##__VA_ARGS__)
#define pr_crit(subsys, fmt, ...)  klog_emit(LOG_CRIT,  (subsys), (fmt), ##__VA_ARGS__)

/* ── Per-subsystem debug helpers ──────────────────────────────────────
 *
 * Subsystems may define a local convenience macro:
 *
 *   #define drm_dbg(fmt, ...) pr_debug("drm", fmt, ##__VA_ARGS__)
 *
 * and gate further via their own CONFIG_DEBUG_<SUBSYS>.  The compile-
 * time gate lives in the per-subsystem .c file so a single subsystem
 * can be turned up without rebuilding everything.
 */

/* In-memory ring sizing — enough to capture the last second of steady-
 * state kernel activity at typical log density.  Sized up-front; never
 * allocated at runtime. */
#define KLOG_RING_ENTRIES   256
#define KLOG_RING_LINELEN   192
