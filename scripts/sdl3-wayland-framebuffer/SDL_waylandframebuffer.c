/*
  MakaOS addition: a native wl_shm software framebuffer for SDL3's Wayland
  video backend.

  Upstream SDL3 3.2.0 implements no CreateWindowFramebuffer on Wayland, so
  SDL_GetWindowSurface() (and therefore the software SDL_Renderer, which calls
  it via SW_ActivateRenderer) only works through the generic texture fallback
  SDL_CreateWindowTexture -- which needs a GPU render driver.  On a system
  without GL/GLES/Vulkan (MakaOS) there is then no software path at all, and
  SDL_CreateRenderer() fails with "Couldn't find matching render driver".

  These three functions give the Wayland window a real wl_shm-backed
  framebuffer, so software rendering works with no GPU: the client draws
  straight into the shared buffer, and UpdateWindowFramebuffer attaches +
  damages + commits it.  The target here is a copy-on-commit compositor
  (wlroots/sway), for which a single buffer is correct; a released-buffer
  pool would be the refinement for compositors that hold client shm buffers
  across frames (tear-free animation).
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_WAYLAND

#include "../SDL_sysvideo.h"
#include "SDL_waylandvideo.h"
#include "SDL_waylandwindow.h"
#include "SDL_waylandshmbuffer.h"
#include "SDL_waylanddyn.h"
#include "SDL_waylandframebuffer.h"

bool Wayland_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                     SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_WindowData *data = window->internal;
    struct Wayland_SHMBuffer *fb;
    int w = 0, h = 0;

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return SDL_SetError("Invalid window size for framebuffer");
    }

    // Drop any previous framebuffer (this is also called again on resize).
    Wayland_DestroyWindowFramebuffer(_this, window);

    fb = (struct Wayland_SHMBuffer *)SDL_calloc(1, sizeof(*fb));
    if (!fb) {
        return false;
    }
    if (!Wayland_AllocSHMBuffer(w, h, fb)) {
        SDL_free(fb);
        return SDL_SetError("Failed to allocate Wayland SHM framebuffer");
    }

    data->shm_framebuffer = fb;
    *format = SDL_PIXELFORMAT_ARGB8888;   // matches WL_SHM_FORMAT_ARGB8888
    *pixels = fb->shm_data;
    *pitch = w * 4;
    return true;
}

bool Wayland_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                     const SDL_Rect *rects, int numrects)
{
    SDL_WindowData *data = window->internal;
    struct Wayland_SHMBuffer *fb = data ? data->shm_framebuffer : NULL;

    (void)_this;
    (void)rects;
    (void)numrects;
    if (!fb || !fb->wl_buffer || !data->surface) {
        return SDL_SetError("No Wayland framebuffer to present");
    }

    wl_surface_attach(data->surface, fb->wl_buffer, 0, 0);
    if (wl_compositor_get_version(data->waylandData->compositor) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
        wl_surface_damage_buffer(data->surface, 0, 0, SDL_MAX_SINT32, SDL_MAX_SINT32);
    } else {
        wl_surface_damage(data->surface, 0, 0, SDL_MAX_SINT32, SDL_MAX_SINT32);
    }
    wl_surface_commit(data->surface);
    wl_display_flush(data->waylandData->display);
    return true;
}

void Wayland_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *data = window->internal;

    (void)_this;
    if (data && data->shm_framebuffer) {
        Wayland_ReleaseSHMBuffer(data->shm_framebuffer);
        SDL_free(data->shm_framebuffer);
        data->shm_framebuffer = NULL;
    }
}

#endif // SDL_VIDEO_DRIVER_WAYLAND
