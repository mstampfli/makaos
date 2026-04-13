#include "initcall.h"
#include "common.h"

// Defined by the linker script.
extern initcall_fn_t __initcall_early_start[];
extern initcall_fn_t __initcall_early_end[];
extern initcall_fn_t __initcall_subsys_start[];
extern initcall_fn_t __initcall_subsys_end[];

static void run_table(initcall_fn_t* start, initcall_fn_t* end) {
    for (initcall_fn_t* fn = start; fn < end; fn++) {
        if (!*fn) continue;
        int ret = (*fn)();
        (void)ret;  // failures are non-fatal; drivers log their own errors
    }
}

void do_initcalls_early(void) {
    run_table(__initcall_early_start, __initcall_early_end);
}

void do_initcalls_subsys(void) {
    run_table(__initcall_subsys_start, __initcall_subsys_end);
}
