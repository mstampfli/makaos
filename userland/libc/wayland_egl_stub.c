// ── Stub implementations for wayland-egl ─────────────────────────────
//
// The real wayland-egl library is part of Mesa and exposes these four
// entry points for compositors' EGL-backed windows.  MakaOS ships
// without Mesa (software-renderer path for SDL3, wlroots software
// pixman renderer).  SDL3's dyn-loader shim still references the
// symbols unconditionally even when EGL is disabled at configure
// time, so we provide stubs that return NULL / abort — they are
// never invoked on our software path.  When Mesa arrives these go
// away along with the stub wayland-egl.h header.

#include <stddef.h>

extern void abort(void);

void* wl_egl_window_create(void* surface, int width, int height) {
    (void)surface; (void)width; (void)height;
    return NULL;
}

void wl_egl_window_destroy(void* egl_window) { (void)egl_window; }

void wl_egl_window_resize(void* egl_window, int w, int h, int dx, int dy) {
    (void)egl_window; (void)w; (void)h; (void)dx; (void)dy;
}

void wl_egl_window_get_attached_size(void* egl_window, int* w, int* h) {
    (void)egl_window;
    if (w) *w = 0;
    if (h) *h = 0;
}
