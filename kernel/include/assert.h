#pragma once
#include "log.h"
#include "panic.h"

/* ── Assertions — DEBUGGING.md §7 ──────────────────────────────────
 *
 * ASSERT(cond)         : invariant check.
 *                         - debug   → panic with condition/file/line
 *                         - release → log a WARN, continue
 * ASSERT_CRIT(cond)    : safety-critical invariant.
 *                         - both builds → panic.  Use when continuing
 *                           would corrupt state or violate a security
 *                           boundary.
 *
 * Messages always include file, line, and the condition text.  Extra
 * context via the &&-trick (spec §7.2):
 *   ASSERT(q->head != NULL && "runqueue head invariant violated");
 *
 * Asserts cover *internal invariants*.  External input gets validated
 * and returns -errno — never assert on syscall/user/hardware input.
 *
 * Asserts MUST NOT have side effects — a release build compiling the
 * condition out must still be correct.
 */

#ifndef CONFIG_DEBUG_ASSERT
/* Default: on whenever CONFIG_DEBUG is on.  Split so a release build
 * can keep assertions enabled even while compiling out pr_debug
 * (two distinct choices). */
#define CONFIG_DEBUG_ASSERT CONFIG_DEBUG
#endif

#if CONFIG_DEBUG_ASSERT
#define ASSERT(cond) do {                                                    \
    if (UNLIKELY(!(cond))) {                                                 \
        panic("ASSERT failed: %s  (%s:%u)", #cond, __FILE__, __LINE__);      \
    }                                                                        \
} while (0)
#else
#define ASSERT(cond) do {                                                    \
    if (UNLIKELY(!(cond))) {                                                 \
        pr_warn("assert", "ASSERT failed: %s  (%s:%u)",                      \
                #cond, __FILE__, __LINE__);                                  \
    }                                                                        \
} while (0)
#endif

#define ASSERT_CRIT(cond) do {                                               \
    if (UNLIKELY(!(cond))) {                                                 \
        panic("ASSERT_CRIT failed: %s  (%s:%u)", #cond, __FILE__, __LINE__); \
    }                                                                        \
} while (0)
