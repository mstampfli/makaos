#include "initcall.h"
#include "preempt.h"
#include "common.h"
#include "kheap.h"

// ── Linker-provided section bounds ────────────────────────────────────────
extern initcall_t* __initcall_0_start[];  // INITCALL_LEVEL_EARLY
extern initcall_t* __initcall_0_end[];
extern initcall_t* __initcall_1_start[];  // INITCALL_LEVEL_SUBSYS
extern initcall_t* __initcall_1_end[];

// ── String helpers (no libc in freestanding kernel) ───────────────────────
static int ic_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// ── Serial output helpers (no heap, no tty yet) ───────────────────────────
static void ic_puts(const char* s) {
    while (*s) {
        while (!(inb(0x3F8 + 5) & 0x20));
        outb(0x3F8, *s++);
    }
}

static void ic_put_str_nl(const char* prefix, const char* name) {
    ic_puts(prefix);
    ic_puts(name);
    ic_puts("\n");
}

// ── DAG state ─────────────────────────────────────────────────────────────
// No fixed cap — node arrays are kmalloc'd based on the actual size of the
// linker-provided initcall section.  kheap is already initialised before
// do_initcalls_early runs (see main.c: kheap_init → do_initcalls_early).

typedef struct {
    initcall_t* ic;
    uint32_t    in_degree;   // number of unresolved dependencies
    uint8_t     done;        // 1 once executed
} ic_node_t;

// Kahn's algorithm: topological sort + cycle detection.
// Executes each node in dependency order.
static void run_dag(initcall_t** start, initcall_t** end) {
    uint32_t slot_count = (uint32_t)(end - start);
    if (slot_count == 0) return;

    ic_node_t* nodes = (ic_node_t*)kmalloc((uint64_t)slot_count * sizeof(ic_node_t));
    uint32_t*  queue = (uint32_t*)kmalloc((uint64_t)slot_count * sizeof(uint32_t));
    if (!nodes || !queue) {
        ic_puts("[initcall] PANIC: OOM allocating initcall DAG\n");
        for (;;) __asm__ volatile("hlt");
    }

    // ── Load nodes ────────────────────────────────────────────────────────
    uint32_t n = 0;
    for (initcall_t** p = start; p < end; p++) {
        if (!*p || !(*p)->fn) continue;
        nodes[n].ic        = *p;
        nodes[n].in_degree = 0;
        nodes[n].done      = 0;
        n++;
    }

    // ── Compute in-degrees ────────────────────────────────────────────────
    // For each node, count how many of its declared deps exist in this level.
    for (uint32_t i = 0; i < n; i++) {
        const char** dep = nodes[i].ic->deps;
        while (*dep) {
            // Find the dep in the node list.
            int found = 0;
            for (uint32_t j = 0; j < n; j++) {
                if (j == i) continue;
                if (nodes[j].ic->name &&
                    ic_strcmp(nodes[j].ic->name, *dep) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found)
                nodes[i].in_degree++;
            // Deps that don't exist in this level are silently satisfied
            // (they live in a different level that already ran).
            dep++;
        }
    }

    // ── Kahn's algorithm ──────────────────────────────────────────────────
    uint32_t qhead = 0, qtail = 0;
    uint32_t executed = 0;

    // Seed: all nodes with no dependencies.
    for (uint32_t i = 0; i < n; i++)
        if (nodes[i].in_degree == 0)
            queue[qtail++] = i;

    while (qhead != qtail) {
        uint32_t idx = queue[qhead++];
        ic_node_t* node = &nodes[idx];

        // ── Execute ───────────────────────────────────────────────────────
        ic_put_str_nl("[initcall] ", node->ic->name);

        int preempt_off = node->ic->flags & INITCALL_FLAG_PREEMPT_OFF;
        if (preempt_off) preempt_disable();

        int ret = node->ic->fn();

        if (preempt_off) preempt_enable();

        if (ret != 0) {
            ic_put_str_nl("[initcall] FAILED: ", node->ic->name);
            if (node->ic->flags & INITCALL_FLAG_REQUIRED) {
                ic_puts("[initcall] PANIC: required initcall failed\n");
                for (;;) __asm__ volatile("hlt");
            }
        }

        node->done = 1;
        executed++;

        // Decrement in-degree of nodes that depend on this one.
        for (uint32_t i = 0; i < n; i++) {
            if (nodes[i].done) continue;
            const char** dep = nodes[i].ic->deps;
            while (*dep) {
                if (ic_strcmp(*dep, node->ic->name) == 0) {
                    if (nodes[i].in_degree > 0)
                        nodes[i].in_degree--;
                    if (nodes[i].in_degree == 0)
                        queue[qtail++] = i;
                    break;
                }
                dep++;
            }
        }
    }

    // ── Cycle detection ───────────────────────────────────────────────────
    if (executed < n) {
        ic_puts("[initcall] PANIC: dependency cycle detected!\n");
        ic_puts("[initcall] Unresolved nodes:\n");
        for (uint32_t i = 0; i < n; i++) {
            if (!nodes[i].done)
                ic_put_str_nl("  - ", nodes[i].ic->name);
        }
        for (;;) __asm__ volatile("hlt");
    }

    kfree(queue);
    kfree(nodes);
}

// ── Public API ────────────────────────────────────────────────────────────

void do_initcalls_early(void) {
    run_dag(__initcall_0_start, __initcall_0_end);
}

void do_initcalls_subsys(void) {
    run_dag(__initcall_1_start, __initcall_1_end);
}
