// ── wayland-egl-core.h — MakaOS stub ─────────────────────────────────
//
// Companion header for userland/libc/wayland_egl_stub.c.  The real
// header ships with Mesa, which MakaOS does not port (software-pixman
// rendering only).  SDL3's Wayland dyn-loader shim includes it
// unconditionally even with EGL disabled, so the declarations must
// exist; the symbols resolve to the no-op stubs in libc.a and are
// never invoked on the software path.  Goes away when Mesa arrives.

#ifndef WAYLAND_EGL_CORE_H
#define WAYLAND_EGL_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

struct wl_surface;
struct wl_egl_window;

struct wl_egl_window* wl_egl_window_create(struct wl_surface* surface,
                                           int width, int height);
void wl_egl_window_destroy(struct wl_egl_window* egl_window);
void wl_egl_window_resize(struct wl_egl_window* egl_window,
                          int width, int height, int dx, int dy);
void wl_egl_window_get_attached_size(struct wl_egl_window* egl_window,
                                     int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif // WAYLAND_EGL_CORE_H
