/*
  MakaOS addition: native wl_shm software framebuffer for the Wayland backend.
  See SDL_waylandframebuffer.c for why this exists.
*/
#include "SDL_internal.h"

#ifndef SDL_waylandframebuffer_h_
#define SDL_waylandframebuffer_h_

extern bool Wayland_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                            SDL_PixelFormat *format, void **pixels, int *pitch);
extern bool Wayland_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window,
                                            const SDL_Rect *rects, int numrects);
extern void Wayland_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window);

#endif // SDL_waylandframebuffer_h_
