/* ── Tracepoints + event ring — implementation ─────────────────────
 *
 * Per DEBUGGING.md §4.  One ring per CPU, lock-free producer on the
 * owning CPU (no contention), readers drain under the panic path with
 * all other CPUs halted.
 *
 * The ring is a power-of-two-sized circular buffer indexed by a
 * monotonically-increasing `next_seq` counter.  Slot i's `seq` field
 * tells the reader whether the slot is live (seq matches) or has
 * been recycled.  This design is the same lock-free pattern used by
 * Linux's per-CPU trace buffers — fast for producers, tolerant of
 * racy readers.
 */

#include "trace.h"
#include "common.h"
#include "cpu.h"
#include "kprintf.h"
#include "tsc.h"

/* ── Tag .rodata pointers ────────────────────────────────────────── */

#define DEF_TAG(name, text) const char* const name = text

DEF_TAG(TRACE_SCHED_SWITCH,    "sched_switch");

DEF_TAG(TRACE_SYSCALL_ENTER,   "syscall_enter");
DEF_TAG(TRACE_SYSCALL_EXIT,    "syscall_exit");

DEF_TAG(TRACE_IPC_SEND,        "ipc_send");
DEF_TAG(TRACE_IPC_RECV,        "ipc_recv");
DEF_TAG(TRACE_IPC_REPLY,       "ipc_reply");

DEF_TAG(TRACE_VM_FAULT,        "vm_fault");
DEF_TAG(TRACE_VM_MAP,          "vm_map");
DEF_TAG(TRACE_VM_UNMAP,        "vm_unmap");

DEF_TAG(TRACE_FS_READ,         "fs_read");
DEF_TAG(TRACE_FS_WRITE,        "fs_write");
DEF_TAG(TRACE_FS_OPEN,         "fs_open");
DEF_TAG(TRACE_FS_CLOSE,        "fs_close");

DEF_TAG(TRACE_DRM_IOCTL,       "drm_ioctl");
DEF_TAG(TRACE_DRM_COMMIT,      "drm_commit");
DEF_TAG(TRACE_DRM_ADDFB,       "drm_addfb");
DEF_TAG(TRACE_DRM_RMFB,        "drm_rmfb");

DEF_TAG(TRACE_GPU_SET_SCANOUT, "gpu_set_scanout");
DEF_TAG(TRACE_GPU_RES_FLUSH,   "gpu_res_flush");
DEF_TAG(TRACE_GPU_RES_TRANSFER,"gpu_res_xfer");

DEF_TAG(TRACE_SIGNAL_DELIVER,  "signal_deliver");

/* ── Ring storage ──────────────────────────────────────────────────
 *
 * One ring per possible CPU.  The per-CPU cpu_t currently doesn't
 * embed the ring (that would balloon cpu.h across the whole tree);
 * instead we key by cpu->id.  Each ring is cache-line padded to
 * avoid false sharing between writers on different CPUs.
 */

typedef struct __attribute__((aligned(64))) {
    uint64_t          next_seq;                 /* monotonic — wraps at 2^64 */
    uint8_t           _pad[64 - sizeof(uint64_t)];
    trace_event_t     slots[TRACE_RING_CAPACITY];
} trace_ring_t;

#define TRACE_MAX_CPUS 64  /* must match smp.h MAX_CPUS */

static trace_ring_t s_rings[TRACE_MAX_CPUS];

void trace_emit(const char* tag,
                 uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint32_t cid = 0;
    uint64_t self_gs;
    __asm__ volatile ("mov %%gs:0,%0" : "=r"(self_gs));
    if (self_gs) cid = this_cpu()->id;
    if (cid >= TRACE_MAX_CPUS) cid = 0;

    trace_ring_t* r = &s_rings[cid];

    /* Producer is the only writer on this CPU; relaxed add is
     * sufficient because (a) no other CPU writes this ring, and
     * (b) the reader re-checks seq after copying. */
    uint64_t seq = __atomic_fetch_add(&r->next_seq, 1, __ATOMIC_RELAXED) + 1;
    uint32_t idx = (uint32_t)((seq - 1) & (TRACE_RING_CAPACITY - 1));
    trace_event_t* e = &r->slots[idx];

    e->ns   = tsc_read_ns();
    e->tag  = tag;
    e->a0   = a0;
    e->a1   = a1;
    e->a2   = a2;
    e->a3   = a3;
    e->cpu  = cid;
    /* Release-store seq last so a concurrent reader that has
     * already snapped the other fields sees a consistent value
     * once seq matches. */
    __atomic_store_n(&e->seq, (uint32_t)seq, __ATOMIC_RELEASE);
}

void trace_ring_dump(void) {
    kprintf_atomic("=== trace ring (per-CPU, oldest first) ===\n");
    for (uint32_t cpu = 0; cpu < TRACE_MAX_CPUS; cpu++) {
        trace_ring_t* r = &s_rings[cpu];
        uint64_t next = __atomic_load_n(&r->next_seq, __ATOMIC_ACQUIRE);
        if (next == 0) continue;  /* CPU never emitted — likely not online */
        uint64_t first = next > TRACE_RING_CAPACITY ? next - TRACE_RING_CAPACITY : 1;

        kprintf_atomic("--- CPU%u: emitted %lu events ---\n",
                        cpu, (uint64_t)next);

        for (uint64_t seq = first; seq < next; seq++) {
            uint32_t idx = (uint32_t)((seq - 1) & (TRACE_RING_CAPACITY - 1));
            trace_event_t* e = &r->slots[idx];
            uint32_t s = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
            /* seq wraps at 2^32 within the slot field, so equality is
             * meaningful modulo 2^32 — low 32 bits of `seq` should
             * match. */
            if (s != (uint32_t)seq) continue;
            uint64_t sec = e->ns / 1000000000ULL;
            uint64_t us  = (e->ns / 1000ULL) % 1000000ULL;
            kprintf_atomic("  [%6lu.%06lu] [CPU%u] %s  "
                            "a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx\n",
                            sec, us, e->cpu,
                            e->tag ? e->tag : "?",
                            e->a0, e->a1, e->a2, e->a3);
        }
    }
    kprintf_atomic("=== end trace ring ===\n");
}
