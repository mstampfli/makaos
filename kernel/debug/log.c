/* ── Structured kernel logging — implementation ───────────────────────
 *
 * Per DEBUGGING.md §2.  Formats every line into a caller-local buffer
 * first, then emits it atomically under the serial lock AND appends
 * it to the in-memory ring — panic code can then dump the tail of the
 * ring (§3.2 item 6) even when serial was behind.
 */

#include "log.h"
#include "kprintf.h"
#include "common.h"
#include "cpu.h"
#include "tsc.h"

/* Freestanding kernel — use compiler builtins for va_list. */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)

extern void* memcpy(void* dst, const void* src, size_t n);
extern void* memset(void* dst, int c, size_t n);

/* ── Ring storage ──────────────────────────────────────────────────── */

typedef struct {
    char text[KLOG_RING_LINELEN];  /* NUL-terminated, includes trailing '\n' */
    uint16_t len;                   /* bytes actually used (excl. NUL) */
    uint16_t _pad;
    /* atomic via __atomic_store_n / __atomic_load_n; plain integer
     * type because clang rejects __atomic_* on _Atomic-qualified
     * integers in freestanding mode. */
    uint32_t seq;                   /* monotonic sequence number */
} klog_slot_t;

static klog_slot_t s_ring[KLOG_RING_ENTRIES];
static uint32_t s_ring_next_seq = 1;  /* 0 = slot unused */

static const char* level_name(log_level_t l) {
    switch (l) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERR:   return "ERR";
    case LOG_CRIT:  return "CRIT";
    }
    return "???";
}

/* Current CPU id — reads %gs:0 → self.  During very early boot before
 * cpu_init_bsp() runs, this_cpu() returns garbage; callers that emit
 * pre-GS-setup go via serial directly (idt.c panic path), so it's fine
 * that we only become correct once GS is set. */
static uint32_t current_cpu_id(void) {
    /* Guard against pre-init calls: if GS hasn't been set, self will
     * be NULL-ish (firmware often leaves 0).  Reading offset 0 off GS
     * then gives 0 which we'd interpret as "*this_cpu == NULL" after
     * the cast — but self is the first field, so the self pointer
     * itself is 0.  Detect that and report CPU? instead of faulting.
     */
    uint64_t self;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(self));
    if (self == 0) return 0xFFFFFFFFu;  /* sentinel for "unknown" */
    return this_cpu()->id;
}

/* Format the canonical prefix into `buf`.  Returns bytes written. */
static int format_prefix(char* buf, size_t cap,
                          log_level_t level, const char* subsys) {
    uint64_t ns   = tsc_read_ns();
    uint64_t secs = ns / 1000000000ULL;
    uint64_t us   = (ns / 1000ULL) % 1000000ULL;
    uint32_t cid  = current_cpu_id();

    /* [    SSSSSS.UUUUUU] [CPU#] [subsys:LEVEL] */
    if (cid == 0xFFFFFFFFu) {
        return ksnprintf(buf, cap,
            "[%6lu.%06lu] [CPU?] [%s:%s] ",
            secs, us, subsys, level_name(level));
    }
    return ksnprintf(buf, cap,
        "[%6lu.%06lu] [CPU%u] [%s:%s] ",
        secs, us, (uint32_t)cid, subsys, level_name(level));
}

/* Append the formatted line to the ring.  The ring is append-only
 * with monotonically-increasing sequence numbers — lock-free via a
 * fetch-add.  On dump we pick up the slots in seq order and stop at
 * the wrap boundary. */
static void ring_append(const char* line, int len) {
    if (len <= 0) return;
    uint32_t seq = __atomic_fetch_add(&s_ring_next_seq, 1, __ATOMIC_RELAXED);
    uint32_t idx = (seq - 1) % KLOG_RING_ENTRIES;
    int cap  = KLOG_RING_LINELEN - 1;
    int copy = len < cap ? len : cap;
    memcpy(s_ring[idx].text, line, (size_t)copy);
    s_ring[idx].text[copy] = 0;
    s_ring[idx].len        = (uint16_t)copy;
    __atomic_store_n(&s_ring[idx].seq, seq, __ATOMIC_RELEASE);
}

/* Emit `line` of `len` bytes to the UART under the serial lock.  The
 * whole line goes out atomically — two CPUs never byte-interleave. */
static void serial_emit_locked(const char* line, int len) {
    uint64_t f = serial_lock_irqsave();
    for (int i = 0; i < len; i++) serial_raw_putc(line[i]);
    serial_unlock_irqrestore(f);
}

/* Raw serial emission with NO lock acquisition — only for the panic
 * path, where all other CPUs are halted and the lock may be held by
 * an aborted writer on this CPU. */
static void serial_emit_unlocked(const char* line, int len) {
    for (int i = 0; i < len; i++) serial_raw_putc(line[i]);
}

void klog_emit(log_level_t level, const char* subsys, const char* fmt, ...) {
    char line[KLOG_RING_LINELEN];

    int pos = format_prefix(line, sizeof(line), level, subsys);

    va_list ap;
    va_start(ap, fmt);
    pos += kvsnprintf(line + pos,
                       pos < (int)sizeof(line) ? sizeof(line) - pos : 0,
                       fmt, ap);
    va_end(ap);

    /* Ensure trailing newline (single line per call). */
    if (pos > (int)sizeof(line) - 2) pos = sizeof(line) - 2;
    line[pos++] = '\n';
    line[pos]   = 0;

    serial_emit_locked(line, pos);
    ring_append(line, pos);
}

void klog_panic_emit(log_level_t level, const char* subsys, const char* fmt, ...) {
    char line[KLOG_RING_LINELEN];

    int pos = format_prefix(line, sizeof(line), level, subsys);

    va_list ap;
    va_start(ap, fmt);
    pos += kvsnprintf(line + pos,
                       pos < (int)sizeof(line) ? sizeof(line) - pos : 0,
                       fmt, ap);
    va_end(ap);

    if (pos > (int)sizeof(line) - 2) pos = sizeof(line) - 2;
    line[pos++] = '\n';
    line[pos]   = 0;

    serial_emit_unlocked(line, pos);
    ring_append(line, pos);
}

void klog_ring_dump(void) {
    /* Walk from the oldest surviving slot forward.  `next_seq` is the
     * sequence number the NEXT emission will use; the oldest surviving
     * entry is thus `next_seq - KLOG_RING_ENTRIES` (or 1 if we haven't
     * wrapped).  Skip any slot whose seq is out of that window — it
     * was either never written or belongs to an earlier wrap. */
    uint32_t next = __atomic_load_n(&s_ring_next_seq, __ATOMIC_ACQUIRE);
    uint32_t first = next > KLOG_RING_ENTRIES ? next - KLOG_RING_ENTRIES : 1;

    /* Unlocked: called from panic with all other CPUs halted. */
    const char* banner = "=== klog ring (oldest first) ===\n";
    for (const char* p = banner; *p; p++) serial_raw_putc(*p);

    for (uint32_t seq = first; seq < next; seq++) {
        uint32_t idx = (seq - 1) % KLOG_RING_ENTRIES;
        if (__atomic_load_n(&s_ring[idx].seq, __ATOMIC_ACQUIRE) != seq) {
            /* Slot was overwritten mid-dump, or we raced; skip. */
            continue;
        }
        const char* line = s_ring[idx].text;
        uint16_t    len  = s_ring[idx].len;
        for (uint16_t i = 0; i < len; i++) serial_raw_putc(line[i]);
    }

    const char* end = "=== end klog ring ===\n";
    for (const char* p = end; *p; p++) serial_raw_putc(*p);
}
