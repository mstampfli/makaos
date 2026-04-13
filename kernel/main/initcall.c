#include "initcall.h"
#include "preempt.h"
#include "common.h"

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
// Maximum initcalls per level.  Increase if needed.
#define INITCALL_MAX_NODES  64

typedef struct {
    initcall_t* ic;
    uint8_t     in_degree;   // number of unresolved dependencies
    uint8_t     done;        // 1 once executed
} ic_node_t;

static ic_node_t s_nodes[INITCALL_MAX_NODES];
static uint8_t   s_n;

// Kahn's algorithm: topological sort + cycle detection.
// Executes each node in dependency order.
static void run_dag(initcall_t** start, initcall_t** end) {
    s_n = 0;

    // ── Load nodes ────────────────────────────────────────────────────────
    for (initcall_t** p = start; p < end; p++) {
        if (!*p || !(*p)->fn) continue;
        if (s_n >= INITCALL_MAX_NODES) {
            ic_puts("[initcall] PANIC: too many initcalls (increase INITCALL_MAX_NODES)\n");
            for (;;) __asm__ volatile("hlt");
        }
        s_nodes[s_n].ic        = *p;
        s_nodes[s_n].in_degree = 0;
        s_nodes[s_n].done      = 0;
        s_n++;
    }

    // ── Compute in-degrees ────────────────────────────────────────────────
    // For each node, count how many of its declared deps exist in this level.
    for (uint8_t i = 0; i < s_n; i++) {
        const char** dep = s_nodes[i].ic->deps;
        while (*dep) {
            // Find the dep in the node list.
            int found = 0;
            for (uint8_t j = 0; j < s_n; j++) {
                if (j == i) continue;
                if (s_nodes[j].ic->name &&
                    ic_strcmp(s_nodes[j].ic->name, *dep) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found)
                s_nodes[i].in_degree++;
            // Deps that don't exist in this level are silently satisfied
            // (they live in a different level that already ran).
            dep++;
        }
    }

    // ── Kahn's algorithm ──────────────────────────────────────────────────
    // Simple queue using a static array — no heap needed.
    uint8_t queue[INITCALL_MAX_NODES];
    uint8_t qhead = 0, qtail = 0;
    uint8_t executed = 0;

    // Seed: all nodes with no dependencies.
    for (uint8_t i = 0; i < s_n; i++)
        if (s_nodes[i].in_degree == 0)
            queue[qtail++ % INITCALL_MAX_NODES] = i;

    while (qhead != qtail) {
        uint8_t idx = queue[qhead++ % INITCALL_MAX_NODES];
        ic_node_t* node = &s_nodes[idx];

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
        for (uint8_t i = 0; i < s_n; i++) {
            if (s_nodes[i].done) continue;
            const char** dep = s_nodes[i].ic->deps;
            while (*dep) {
                if (ic_strcmp(*dep, node->ic->name) == 0) {
                    if (s_nodes[i].in_degree > 0)
                        s_nodes[i].in_degree--;
                    if (s_nodes[i].in_degree == 0)
                        queue[qtail++ % INITCALL_MAX_NODES] = i;
                    break;
                }
                dep++;
            }
        }
    }

    // ── Cycle detection ───────────────────────────────────────────────────
    if (executed < s_n) {
        ic_puts("[initcall] PANIC: dependency cycle detected!\n");
        ic_puts("[initcall] Unresolved nodes:\n");
        for (uint8_t i = 0; i < s_n; i++) {
            if (!s_nodes[i].done)
                ic_put_str_nl("  - ", s_nodes[i].ic->name);
        }
        for (;;) __asm__ volatile("hlt");
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void do_initcalls_early(void) {
    run_dag(__initcall_0_start, __initcall_0_end);
}

void do_initcalls_subsys(void) {
    run_dag(__initcall_1_start, __initcall_1_end);
}
