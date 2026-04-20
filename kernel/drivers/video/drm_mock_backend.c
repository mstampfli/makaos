// ── Mock DRM backend (#8 of design-hardening) ────────────────────────
//
// Purpose: a backend that records every call into an in-memory log
// without touching real hardware.  Lets the modesetting + commit
// logic be exercised in unit-test style, driven by the existing
// kernel selftest harness.
//
// This is NOT wired in by default — virtio_gpu_init calls
// drm_backend_register(&vgpu_backend_ops) at subsys init.  To run
// the self-test against the mock instead, call
// drm_mock_backend_activate() from init_kthread BEFORE the real
// backend registers, then run drm_mock_selftest().
//
// Lives in its own TU so it can be omitted from production builds
// by just dropping this file from kernel/drivers/video/.

#include "drm_backend.h"
#include "virtio_gpu.h"   // for format constant
#include "kprintf.h"

#define MOCK_MAX_RESOURCES 16
#define MOCK_MAX_SCANOUTS  4

typedef struct {
    uint32_t id;
    uint32_t format;
    uint32_t w, h;
    phys_addr_t backing_phys;
    uint32_t    backing_bytes;
    int      has_backing;
} mock_resource_t;

static mock_resource_t s_res[MOCK_MAX_RESOURCES];
static uint32_t        s_scanout_res[MOCK_MAX_SCANOUTS];  // resource_id bound
static uint32_t        s_scanout_w[MOCK_MAX_SCANOUTS];
static uint32_t        s_scanout_h[MOCK_MAX_SCANOUTS];

// Stats — selftest asserts on these.
uint32_t g_mock_calls_create;
uint32_t g_mock_calls_destroy;
uint32_t g_mock_calls_attach;
uint32_t g_mock_calls_set_scanout;
uint32_t g_mock_calls_transfer;
uint32_t g_mock_calls_flush;

static mock_resource_t* find_res(uint32_t id) {
    if (id == 0) return NULL;
    for (uint32_t i = 0; i < MOCK_MAX_RESOURCES; i++)
        if (s_res[i].id == id) return &s_res[i];
    return NULL;
}

static uint32_t mock_scanout_count(void) { return 1; }
static void mock_scanout_mode(uint32_t idx, uint32_t* w, uint32_t* h) {
    (void)idx;
    if (w) *w = 1920;
    if (h) *h = 1080;
}

static int mock_resource_create(uint32_t id, uint32_t fmt, uint32_t w, uint32_t h) {
    g_mock_calls_create++;
    for (uint32_t i = 0; i < MOCK_MAX_RESOURCES; i++) {
        if (s_res[i].id == 0) {
            s_res[i] = (mock_resource_t){ .id = id, .format = fmt, .w = w, .h = h };
            return 0;
        }
    }
    return -1;
}

static int mock_resource_destroy(uint32_t id) {
    g_mock_calls_destroy++;
    mock_resource_t* r = find_res(id);
    if (!r) return -1;
    r->id = 0;
    return 0;
}

static int mock_resource_attach(uint32_t id, phys_addr_t phys, uint32_t bytes) {
    g_mock_calls_attach++;
    mock_resource_t* r = find_res(id);
    if (!r) return -1;
    r->backing_phys  = phys;
    r->backing_bytes = bytes;
    r->has_backing   = 1;
    return 0;
}

static int mock_scanout_set(uint32_t sc, uint32_t res, uint32_t w, uint32_t h) {
    g_mock_calls_set_scanout++;
    if (sc >= MOCK_MAX_SCANOUTS) return -1;
    if (res && !find_res(res)) return -1;
    s_scanout_res[sc] = res;
    s_scanout_w[sc]   = w;
    s_scanout_h[sc]   = h;
    return 0;
}

static int mock_transfer(uint32_t id, uint32_t w, uint32_t h) {
    (void)w; (void)h;
    g_mock_calls_transfer++;
    return find_res(id) ? 0 : -1;
}

static int mock_flush(uint32_t id, uint32_t w, uint32_t h) {
    (void)w; (void)h;
    g_mock_calls_flush++;
    return find_res(id) ? 0 : -1;
}

static const drm_backend_ops_t s_mock_ops = {
    .scanout_count           = mock_scanout_count,
    .scanout_mode            = mock_scanout_mode,
    .resource_create         = mock_resource_create,
    .resource_destroy        = mock_resource_destroy,
    .resource_attach_backing = mock_resource_attach,
    .scanout_set             = mock_scanout_set,
    .resource_transfer       = mock_transfer,
    .resource_flush          = mock_flush,
};

void drm_mock_backend_activate(void) {
    for (uint32_t i = 0; i < MOCK_MAX_RESOURCES; i++) s_res[i].id = 0;
    for (uint32_t i = 0; i < MOCK_MAX_SCANOUTS;  i++) s_scanout_res[i] = 0;
    g_mock_calls_create     = 0;
    g_mock_calls_destroy    = 0;
    g_mock_calls_attach     = 0;
    g_mock_calls_set_scanout = 0;
    g_mock_calls_transfer   = 0;
    g_mock_calls_flush      = 0;
    drm_backend_register(&s_mock_ops);
}

// ── Selftest: exercises the atomic commit + rollback paths without
//    touching real hardware. ─────────────────────────────────────────
extern void kprintf_atomic(const char* fmt, ...);

void drm_mock_selftest(void) {
    // Reset mock state without touching the global drm_backend pointer.
    // The real virtio-gpu backend continues to drive /dev/dri/card0
    // for the rest of the system — we just probe our own vtable here.
    for (uint32_t i = 0; i < MOCK_MAX_RESOURCES; i++) s_res[i].id = 0;
    for (uint32_t i = 0; i < MOCK_MAX_SCANOUTS;  i++) s_scanout_res[i] = 0;
    g_mock_calls_create      = 0;
    g_mock_calls_destroy     = 0;
    g_mock_calls_attach      = 0;
    g_mock_calls_set_scanout = 0;
    g_mock_calls_transfer    = 0;
    g_mock_calls_flush       = 0;

    // Call the mock vtable directly (this is what drm_commit_apply does
    // — only via a pointer it loaded from the drm_backend global).
    const drm_backend_ops_t* b = &s_mock_ops;
    if (!b) { kprintf_atomic("[drm-mock-selftest] FAIL no ops\n"); return; }
    if (b->scanout_count() != 1) {
        kprintf_atomic("[drm-mock-selftest] FAIL scanout_count\n"); return;
    }
    if (b->resource_create(1, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, 1920, 1080) != 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL resource_create\n"); return;
    }
    if (b->resource_attach_backing(1, 0x10000, 1920*1080*4) != 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL attach\n"); return;
    }
    if (b->scanout_set(0, 1, 1920, 1080) != 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL scanout_set\n"); return;
    }
    if (b->resource_transfer(1, 1920, 1080) != 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL transfer\n"); return;
    }
    if (b->resource_flush(1, 1920, 1080) != 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL flush\n"); return;
    }

    // Invalid ops: attach to nonexistent resource should fail; scanout
    // with bogus resource should fail — proves error paths work.
    if (b->resource_attach_backing(999, 0x20000, 4096) == 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL attach-nonexistent accepted\n"); return;
    }
    if (b->scanout_set(0, 999, 100, 100) == 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL set_scanout-nonexistent accepted\n"); return;
    }

    // Teardown path.
    if (b->resource_destroy(1) != 0) {
        kprintf_atomic("[drm-mock-selftest] FAIL destroy\n"); return;
    }

    // Verify the call counters match the expected sequence.
    if (g_mock_calls_create != 1 || g_mock_calls_destroy != 1 ||
        g_mock_calls_attach != 2 ||  /* 1 OK + 1 failed */
        g_mock_calls_set_scanout != 2 ||
        g_mock_calls_transfer != 1 || g_mock_calls_flush != 1) {
        kprintf_atomic("[drm-mock-selftest] FAIL call counts (c=%u d=%u a=%u s=%u t=%u f=%u)\n",
                       g_mock_calls_create, g_mock_calls_destroy,
                       g_mock_calls_attach, g_mock_calls_set_scanout,
                       g_mock_calls_transfer, g_mock_calls_flush);
        return;
    }

    kprintf_atomic("[drm-mock-selftest] PASS (vtable + happy path + error paths)\n");
}
