#include "initcall.h"
#include "preempt.h"
#include "common.h"
#include "tty.h"
#include "evdev.h"
#include "ahci.h"
#include "nvme.h"
#include "ext2.h"
#include "keyboard.h"
#include "mouse.h"
#include "hda.h"
#include "net/net.h"
#include "fb.h"
#include "ioapic.h"
#include "drivers/video/virtio_gpu.h"

// ── INITCALL_LEVEL_EARLY registrations ───────────────────────────────────
// No sleeping.  Strict dependency order declared explicitly.

static int _tty_init(void)   { tty_init();        return 0; }
static int _evdev_init(void) { evdev_init();       return 0; }
static int _ahci_init(void)  { ahci_init();        return 0; }
static int _ext2_init(void)  {
    extern void dcache_init(void);
    dcache_init();           // Phase 7A: dentry cache must exist
                             // before first path walk.
    ext2_init(4096);
    return 0;
}

// tty: no kernel deps at this level (tty_init only needs the heap)
DEFINE_INITCALL(tty, INITCALL_LEVEL_EARLY, .fn = _tty_init);

// evdev: registers with input_core which is part of tty subsystem
DEFINE_INITCALL(evdev, INITCALL_LEVEL_EARLY,
    .fn   = _evdev_init,
    .deps = INITCALL_DEPS("tty"),
);

// ahci: needs PCI (already done in kmain before do_initcalls_early)
DEFINE_INITCALL(ahci, INITCALL_LEVEL_EARLY, .fn = _ahci_init);

// nvme: also needs PCI; runs after ahci so boot paths are discovered in
// a consistent order.  Returns gracefully if no NVMe controller exists.
static int _nvme_init(void) { nvme_init(); return 0; }
DEFINE_INITCALL(nvme, INITCALL_LEVEL_EARLY,
    .fn   = _nvme_init,
    .deps = INITCALL_DEPS("ahci"),
);

// ext2: needs ahci to be initialised first
DEFINE_INITCALL(ext2, INITCALL_LEVEL_EARLY,
    .fn   = _ext2_init,
    .deps = INITCALL_DEPS("ahci"),
);

// ── INITCALL_LEVEL_SUBSYS registrations ──────────────────────────────────
// Process context — can sleep.

// input: keyboard + mouse init + full flush.  Runs in process context
// with preemption enabled — each device driver handles its own short
// preempt_disable sections internally where hardware access needs to
// be atomic.
static int _input_init(void) {
    // keyboard_init installs IRQ1 handler but keeps IRQ1 masked —
    // mouse_init polls port 0x60 for ACKs and IRQ1 would steal those bytes.
    // After mouse_init completes, flush the KBC+FIFO then unmask IRQ1.
    // mouse_init unmasks IRQ12 itself (no interference with keyboard path).
    keyboard_init();
    mouse_init();
    // virtio-input absolute tablet (QEMU -device virtio-tablet-pci) —
    // registers event2 if present; absence is fine (PS/2 still works).
    extern int virtio_input_init(void);
    virtio_input_init();
    keyboard_flush();
    ioapic_unmask(ioapic_isa_to_gsi(1));
    tty_flush_input(&g_tty0);
    fb_clear();
    return 0;
}

DEFINE_INITCALL(input, INITCALL_LEVEL_SUBSYS,
    .fn   = _input_init,
    .deps = INITCALL_DEPS("tty", "evdev"),
);

// hda: Intel HDA audio — may sleep waiting for codec response
static int _hda_init(void) { hda_init(); return 0; }

DEFINE_INITCALL(hda, INITCALL_LEVEL_SUBSYS, .fn = _hda_init);

// net: virtio-net + IP stack — may sleep waiting for link negotiation
static int _net_init(void) { net_init(); return 0; }

DEFINE_INITCALL(net, INITCALL_LEVEL_SUBSYS, .fn = _net_init);

// virtio-gpu: PCI-based 2D scanout device.  Silently no-ops if the
// device isn't present (hardware boot, QEMU without -device virtio-gpu).
// Runs in subsys level because feature negotiation + GET_DISPLAY_INFO
// may involve delays — the probe polls the control-queue used ring.
// After init, register as the DRM core's backend so /dev/dri/card0
// ioctls route here.
static int _virtio_gpu_init(void) {
    if (!virtio_gpu_init()) return 0;
    virtio_gpu_register_backend();

    // Repoint the text console onto a virtio-gpu resource so the
    // kernel owns its scanout buffer rather than relying on UEFI GOP
    // memory being mirrored by QEMU's virtio-vga VGA-compat BAR (it
    // isn't — SET_SCANOUT(res_id=0) leaves the display blanked).
    // On success: copy the current GOP contents into the new backing
    // so boot-time prints don't disappear, then hand the new buffer
    // to fb_init and install the flush hook.  On failure: the
    // legacy GOP mapping keeps being used — display goes dark when
    // a DRM client exits, but boot still proceeds.
    phys_addr_t phys = 0;
    uint8_t*    virt = NULL;
    uint32_t    w = 0, h = 0, pitch = 0;
    if (virtio_gpu_fbcon_init(&phys, &virt, &w, &h, &pitch)) {
        // Best-effort GOP → virtio-gpu blit.  Row-by-row because the
        // two backings have potentially different pitches (GOP pitch
        // ≠ w*4 in general, virtio-gpu backing is packed).
        uint8_t* src = (uint8_t*)g_fb.base_virt;
        if (src && g_fb.width && g_fb.height && g_fb.pitch) {
            uint32_t rows_copy = g_fb.height < h ? g_fb.height : h;
            uint32_t bytes_per_row = (g_fb.width < w ? g_fb.width : w) * 4u;
            for (uint32_t y = 0; y < rows_copy; y++) {
                __builtin_memcpy(virt + (uint64_t)y * pitch,
                                 src  + (uint64_t)y * g_fb.pitch,
                                 bytes_per_row);
            }
        }
        // Preserve the text cursor across the repoint so the next
        // kprintf continues below the carried-over boot text instead
        // of overwriting it from the top-left.
        uint32_t saved_col = g_fb_col, saved_row = g_fb_row;
        uint32_t saved_fg  = g_fb_fg,  saved_bg  = g_fb_bg;
        fb_init((uint64_t)phys, w, h, pitch);
        g_fb_col = saved_col;
        g_fb_row = saved_row;
        g_fb_fg  = saved_fg;
        g_fb_bg  = saved_bg;
        fb_set_flush_hook(virtio_gpu_fbcon_flush);
        // Push the carried-over boot text to the host immediately.
        virtio_gpu_fbcon_flush();
    }
    return 0;
}

DEFINE_INITCALL(virtio_gpu, INITCALL_LEVEL_SUBSYS, .fn = _virtio_gpu_init);
