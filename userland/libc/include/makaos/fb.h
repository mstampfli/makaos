#ifndef _MAKAOS_FB_H
#define _MAKAOS_FB_H 1
// Framebuffer access — root-only mapping into userspace.
// Used by the compositor, makadisplay, and anything doing direct pixel
// output.  The FB is 32-bit BGRA on x86_64 UEFI/GOP.

#include <stdint.h>

typedef struct {
    uint64_t phys;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;        // bytes per row (may exceed width*4 due to alignment)
    uint32_t bpp;          // 32
} fb_info_t;

int    fb_info(fb_info_t* out);
void*  fb_map(void);                     // returns pointer to mapped FB, NULL on failure
int    fb_blit(const void* src,
                uint32_t dst_x, uint32_t dst_y,
                uint32_t w, uint32_t h, uint32_t src_pitch);

#endif
