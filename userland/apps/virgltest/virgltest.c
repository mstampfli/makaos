/*
 * virgltest -- exercises the virtio-gpu 3D render-node uAPI end to end from
 * USERLAND (docs/VIRGL_BRINGUP.md phase 2a-2c).  Opens /dev/dri/renderD128 and:
 *   GETPARAM(3D_FEATURES) -> CONTEXT_INIT(capset VIRGL2) -> RESOURCE_CREATE a
 *   16x16 BGRA render target -> MAP + mmap its backing -> EXECBUFFER a virgl
 *   command stream that CLEARs it to magenta -> TRANSFER_FROM_HOST -> WAIT,
 * then reads the mmap'd pixel and checks it is exactly magenta (0xFFFF00FF).
 * That means the host GPU rendered on our behalf, driven purely through the
 * DRM ioctl uAPI Mesa's virgl driver uses.
 *
 * Phase 2c: PRIME-share the GPU-rendered buffer to a SECOND fd (the real
 * Mesa-render-node -> compositor path): export the res3d as a dma-buf, import
 * it on a fresh fd as a GEM handle, MAP_DUMB + mmap that handle and confirm it
 * sees the SAME magenta -- zero-copy cross-fd sharing.
 *
 * Every ioctl is also driven off its happy path (16 reject cases: bad handles,
 * bad sizes, missing context, double context-init, non-PRIME import, ...) and
 * asserted to fail-closed with the EXACT errno -- a wrongly-accepted bad input
 * fails the test, so the reject branches are proven, not assumed.
 */
#include "libc.h"   // open/close/mmap/munmap/ioctl/printf + O_RDWR/PROT_*/MAP_SHARED + errno/E*

/* ioctl numbers -- byte-exact with libdrm drm/virtgpu_drm.h (see drm.c). */
#define DRM_IOCTL_VIRTGPU_MAP               0xC0106441u
#define DRM_IOCTL_VIRTGPU_EXECBUFFER        0xC0406442u
#define DRM_IOCTL_VIRTGPU_GETPARAM          0xC0106443u
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE   0xC0386444u
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO     0xC0106445u
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST 0xC02C6446u
#define DRM_IOCTL_VIRTGPU_WAIT              0xC0086448u
#define DRM_IOCTL_VIRTGPU_GET_CAPS          0xC0186449u
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT      0xC010644Bu
#define DRM_IOCTL_PRIME_HANDLE_TO_FD        0xC00C642Du
#define DRM_IOCTL_PRIME_FD_TO_HANDLE        0xC00C642Eu
#define DRM_IOCTL_MODE_MAP_DUMB             0xC01064B3u

#define VIRTGPU_PARAM_3D_FEATURES        1
#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID  0x0001
#define VIRTGPU_DRM_CAPSET_VIRGL2        2

struct dvg_getparam { unsigned long long param, value; };
struct dvg_ctx_set_param { unsigned long long param, value; };
struct dvg_context_init { unsigned int num_params, pad; unsigned long long ctx_set_params; };
struct dvg_resource_create {
    unsigned int target, format, bind, width, height, depth, array_size,
                 last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct dvg_map { unsigned long long offset; unsigned int handle, pad; };
struct dvg_map_dumb { unsigned int handle, pad; unsigned long long offset; };
struct dvg_resource_info { unsigned int bo_handle, res_handle, size, blob_mem; };
struct dvg_get_caps { unsigned int cap_set_id, cap_set_ver; unsigned long long addr;
                      unsigned int size, pad; };
struct dvg_execbuffer {
    unsigned int flags, size; unsigned long long command, bo_handles;
    unsigned int num_bo_handles; int fence_fd; unsigned int ring_idx, syncobj_stride;
    unsigned int num_in_syncobjs, num_out_syncobjs; unsigned long long in_syncobjs, out_syncobjs;
};
struct dvg_3d_box { unsigned int x, y, z, w, h, d; };
struct dvg_transfer { unsigned int bo_handle; struct dvg_3d_box box;
                      unsigned int level, offset, stride, layer_stride; };
struct dvg_wait { unsigned int handle, flags; };
struct dvg_prime { unsigned int handle, flags; int fd; };

/* virgl encodings (virgl_protocol.h / virgl_hw.h). */
#define VIRGL_CMD0(cmd, obj, len)         ((unsigned int)((cmd) | ((obj) << 8) | ((len) << 16)))
#define VIRGL_CCMD_CREATE_OBJECT          1u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE  5u
#define VIRGL_CCMD_CLEAR                  7u
#define VIRGL_OBJECT_SURFACE              8u
#define VIRGL_FORMAT_B8G8R8A8_UNORM       1u
#define PIPE_TEXTURE_2D                   2u
#define VIRGL_BIND_RENDER_TARGET          (1u << 1)
#define PIPE_CLEAR_COLOR0                 (1u << 2)

#define W 16u
#define H 16u
#define SURF 1u
#define MAGENTA 0xFFFF00FFu

/* A reject probe: the ioctl MUST fail (-1) with the EXACT expected errno.
   Returns 1 if it fail-closed correctly, 0 otherwise (and logs the miss). */
static int expect_err(const char* what, int ret, int experr) {
    if (ret == 0) { printf("  reject %-18s ACCEPTED bad input (BUG)\n", what); return 0; }
    if (errno != experr) {
        printf("  reject %-18s errno=%d want %d\n", what, errno, experr);
        return 0;
    }
    return 1;
}

int main(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR, 0);
    if (fd < 0) { printf("virgltest: FAIL open renderD128 (%d)\n", fd); return 1; }

    /* 3D available? */
    unsigned long long feat = 0;
    struct dvg_getparam gp = { .param = VIRTGPU_PARAM_3D_FEATURES,
                               .value = (unsigned long long)(unsigned long)&feat };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) != 0 || !feat) {
        printf("virgltest: FAIL 3D not available (feat=%llu)\n", feat); return 1;
    }

    /* Context with the VIRGL2 capset. */
    struct dvg_ctx_set_param cp = { .param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID,
                                    .value = VIRTGPU_DRM_CAPSET_VIRGL2 };
    struct dvg_context_init ci = { .num_params = 1, .pad = 0,
                                   .ctx_set_params = (unsigned long long)(unsigned long)&cp };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci) != 0) {
        printf("virgltest: FAIL CONTEXT_INIT\n"); return 1;
    }

    /* A 16x16 BGRA render target. */
    struct dvg_resource_create rc = {0};
    rc.target = PIPE_TEXTURE_2D; rc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    rc.bind = VIRGL_BIND_RENDER_TARGET; rc.width = W; rc.height = H;
    rc.depth = 1; rc.array_size = 1;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc) != 0 || rc.bo_handle == 0) {
        printf("virgltest: FAIL RESOURCE_CREATE\n"); return 1;
    }

    /* Map + mmap the backing so we can read the result. */
    struct dvg_map mp = { .handle = rc.bo_handle };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &mp) != 0) {
        printf("virgltest: FAIL MAP\n"); return 1;
    }
    unsigned int* px = (unsigned int*)mmap(0, W * H * 4u, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, (long)mp.offset);
    if (px == (void*)-1) { printf("virgltest: FAIL mmap\n"); return 1; }
    for (unsigned i = 0; i < W * H; i++) px[i] = 0xDEADBEEFu;   /* pre-poison */

    /* virgl command stream: surface view -> bind as cbuf0 -> clear magenta. */
    unsigned int cmd[19], n = 0;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    cmd[n++] = SURF; cmd[n++] = rc.res_handle; cmd[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    cmd[n++] = 1; cmd[n++] = 0; cmd[n++] = SURF;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    cmd[n++] = PIPE_CLEAR_COLOR0;
    cmd[n++] = 0x3F800000u; cmd[n++] = 0x00000000u; cmd[n++] = 0x3F800000u; cmd[n++] = 0x3F800000u;
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;

    struct dvg_execbuffer eb = {0};
    eb.size = n * 4u; eb.command = (unsigned long long)(unsigned long)cmd;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb) != 0) {
        printf("virgltest: FAIL EXECBUFFER\n"); return 1;
    }

    struct dvg_transfer tr = {0};
    tr.bo_handle = rc.bo_handle; tr.box.w = W; tr.box.h = H; tr.box.d = 1;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &tr) != 0) {
        printf("virgltest: FAIL TRANSFER_FROM_HOST\n"); return 1;
    }
    struct dvg_wait wt = { .handle = rc.bo_handle };
    ioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &wt);

    /* Verify the GPU cleared it to magenta. */
    unsigned int bad = 0, first = px[0];
    for (unsigned i = 0; i < W * H; i++) if (px[i] != MAGENTA) bad++;

    // ── Phase 2c: cross-fd PRIME round-trip ──────────────────────────────
    // Export the rendered res3d as a dma-buf, import it on a SECOND fd as a
    // fresh GEM handle, mmap that handle, and confirm it sees the SAME magenta
    // -- zero-copy sharing across fds (the Mesa render-node -> compositor path).
    int prime_ok = 0; unsigned int prime_first = 0, prime_bad = W * H;
    int fd2 = open("/dev/dri/renderD128", O_RDWR, 0);
    struct dvg_prime pe = { .handle = rc.bo_handle };
    if (fd2 >= 0 && ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &pe) == 0 && pe.fd >= 0) {
        struct dvg_prime pi = { .fd = pe.fd };
        if (ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &pi) == 0 && pi.handle != 0) {
            struct dvg_map_dumb md = { .handle = pi.handle };
            if (ioctl(fd2, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
                unsigned int* px2 = (unsigned int*)mmap(0, W * H * 4u,
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd2, (long)md.offset);
                if (px2 != (void*)-1) {
                    prime_first = px2[0]; prime_bad = 0;
                    for (unsigned i = 0; i < W * H; i++) if (px2[i] != MAGENTA) prime_bad++;
                    prime_ok = (prime_bad == 0);
                    munmap(px2, W * H * 4u);
                }
            }
        }
        close(pe.fd);
    }

    // ── Non-happy paths: every ioctl must fail-closed on bad input ───────
    // (16 probes; each asserts the EXACT errno so a wrong-but-nonzero return
    //  or an accepted bad input both fail the test.)
    int rj = 0, rjn = 0;
    // GETPARAM into a NULL user pointer -> EFAULT.
    struct dvg_getparam gpe = { .param = VIRTGPU_PARAM_3D_FEATURES, .value = 0 };
    rjn++; rj += expect_err("getparam-null", ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gpe), EFAULT);
    // GET_CAPS with size 0 and oversized -> EINVAL.
    unsigned char capbuf[64];
    struct dvg_get_caps gc = { .cap_set_id = VIRTGPU_DRM_CAPSET_VIRGL2,
                               .addr = (unsigned long long)(unsigned long)capbuf, .size = 0 };
    rjn++; rj += expect_err("getcaps-size0", ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &gc), EINVAL);
    gc.size = 8192;
    rjn++; rj += expect_err("getcaps-huge", ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &gc), EINVAL);
    // CONTEXT_INIT a second time on a fd that already has one -> EEXIST.
    rjn++; rj += expect_err("ctxinit-twice", ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci), EEXIST);
    // RESOURCE_CREATE with zero and oversized dims -> EINVAL.
    struct dvg_resource_create rz = rc; rz.width = 0; rz.bo_handle = 0;
    rjn++; rj += expect_err("rescreate-zero", ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rz), EINVAL);
    rz = rc; rz.width = 20000; rz.bo_handle = 0;
    rjn++; rj += expect_err("rescreate-huge", ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rz), EINVAL);
    // MAP / INFO / WAIT / TRANSFER of a nonexistent handle -> ENOENT.
    struct dvg_map mbad = { .handle = 999u };
    rjn++; rj += expect_err("map-badhandle", ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &mbad), ENOENT);
    struct dvg_resource_info ibad = { .bo_handle = 999u };
    rjn++; rj += expect_err("info-badhandle", ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &ibad), ENOENT);
    struct dvg_wait wbad = { .handle = 999u };
    rjn++; rj += expect_err("wait-badhandle", ioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &wbad), ENOENT);
    struct dvg_transfer tbad = {0}; tbad.bo_handle = 999u; tbad.box.w = W; tbad.box.h = H;
    rjn++; rj += expect_err("xfer-badhandle", ioctl(fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &tbad), ENOENT);
    // EXECBUFFER with zero size and non-word-multiple size -> EINVAL.
    struct dvg_execbuffer ez = {0}; ez.size = 0; ez.command = (unsigned long long)(unsigned long)cmd;
    rjn++; rj += expect_err("exec-zerosize", ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &ez), EINVAL);
    ez.size = 6;
    rjn++; rj += expect_err("exec-misaligned", ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &ez), EINVAL);
    // PRIME export of a nonexistent handle -> ENOENT.
    struct dvg_prime peb = { .handle = 999u };
    rjn++; rj += expect_err("prime-exp-bad", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &peb), ENOENT);
    // PRIME import of our own (non-PRIME) drm fd -> EINVAL; of a bogus fd -> EBADF.
    struct dvg_prime pib = { .fd = fd };
    rjn++; rj += expect_err("prime-imp-notpr", ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &pib), EINVAL);
    struct dvg_prime pibf = { .fd = 999 };
    rjn++; rj += expect_err("prime-imp-badfd", ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &pibf), EBADF);
    // EXECBUFFER / TRANSFER on fd2, which never CONTEXT_INIT'd -> EINVAL (context first).
    struct dvg_execbuffer enc = {0}; enc.size = 4u; enc.command = (unsigned long long)(unsigned long)cmd;
    rjn++; rj += expect_err("exec-noctx", ioctl(fd2, DRM_IOCTL_VIRTGPU_EXECBUFFER, &enc), EINVAL);

    int pass = (bad == 0 && prime_ok && rj == rjn);
    printf("virgltest: render first=%x bad=%u/%u  prime first=%x bad=%u/%u  reject %d/%d -> %s\n",
           first, bad, W * H, prime_first, prime_bad, W * H, rj, rjn,
           pass ? "PASS" : "FAIL");

    // App stdout goes to the VGA console, not the serial log, and the gl
    // launch's console surface isn't screendumpable -- so also signal the
    // results to the kernel serial log via unknown-ioctl request numbers the
    // drm layer pr_warns verbatim ("unknown ioctl req=0x%08x").  Encode the
    // actual counters so the serial log carries the evidence, not just a
    // boolean: 0xBAD0<n> render mismatches, 0xADD0<n> prime mismatches,
    // 0x4A00<rj><rjn> reject probes passed/total, then the verdict.
    ioctl(fd, 0xBAD00000UL | (bad & 0xFFFFu), (void*)0);
    ioctl(fd, 0xADD00000UL | (prime_bad & 0xFFFFu), (void*)0);
    ioctl(fd, 0x4A000000UL | ((unsigned)rj << 8) | ((unsigned)rjn & 0xFFu), (void*)0);
    ioctl(fd, pass ? 0x600D600DUL : 0x0BADBAD0UL, (void*)0);

    munmap(px, W * H * 4u);
    if (fd2 >= 0) close(fd2);
    close(fd);
    return 0;
}
