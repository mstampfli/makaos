// Public header installed at wlr/backend/native.h.  Exposes the
// MakaOS native backend factory so compositors can combine it with
// the DRM backend via wlr_multi_backend.

#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_NATIVE_H
#define WLR_BACKEND_NATIVE_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>

// Create the MakaOS native input backend.  Opens /dev/input/event0
// and registers one keyboard + one pointer.  Owns the fd for its
// lifetime.  Destroy via wlr_backend_destroy on the returned handle.
//
// Returns NULL on failure (logged via wlr_log).
struct wlr_backend* wlr_native_backend_create(struct wl_event_loop* loop);

// True iff `backend` was created by wlr_native_backend_create.
bool wlr_backend_is_native(struct wlr_backend* backend);

#endif
