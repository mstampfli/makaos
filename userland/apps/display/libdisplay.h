#pragma once
#include "../../libc/libc.h"
#include "protocol.h"

// ── MakaDisplay Client Library ───────────────────────────────────────────
//
// Provides a clean C API for connecting to the MakaDisplay compositor,
// creating windows (surfaces), allocating pixel buffers, and receiving
// input events.
//
// Usage:
//   md_display_t* dpy = md_display_connect();
//   md_surface_t* surf = md_surface_create(dpy);
//   md_client_buffer_t* buf = md_buffer_create(dpy, w, h);
//   // render into buf->data ...
//   md_surface_attach(surf, buf);
//   md_surface_commit(surf);
//   // event loop: md_display_dispatch(dpy)

// ── Opaque types ─────────────────────────────────────────────────────────

typedef struct md_display        md_display_t;
typedef struct md_client_surface md_client_surface_t;
typedef struct md_client_buffer  md_client_buffer_t;
typedef struct md_dbuf           md_dbuf_t;

// ── Event callbacks ──────────────────────────────────────────────────────

typedef void (*md_key_handler_t)(md_client_surface_t* surf,
                                uint32_t keycode, uint32_t modifiers,
                                int pressed);
typedef void (*md_configure_handler_t)(md_client_surface_t* surf,
                                      uint32_t width, uint32_t height,
                                      uint32_t states);
typedef void (*md_close_handler_t)(md_client_surface_t* surf);
typedef void (*md_focus_handler_t)(md_client_surface_t* surf, int focused);
typedef void (*md_pointer_move_handler_t)(md_client_surface_t* surf,
                                         int32_t x, int32_t y);
typedef void (*md_pointer_button_handler_t)(md_client_surface_t* surf,
                                           uint32_t button, int pressed);
typedef void (*md_buffer_release_handler_t)(md_client_buffer_t* buf);

// ── Display connection ───────────────────────────────────────────────────

// Connect to the display server.  Returns NULL on failure.
md_display_t* md_display_connect(void);

// Disconnect and free all resources.
void md_display_disconnect(md_display_t* dpy);

// Process pending events (non-blocking).
// Returns number of events processed, or -1 on error/disconnect.
int md_display_dispatch(md_display_t* dpy);

// Block until at least one event is available, then dispatch all.
int md_display_dispatch_blocking(md_display_t* dpy);

// Get the display server's framebuffer dimensions.
uint32_t md_display_width(md_display_t* dpy);
uint32_t md_display_height(md_display_t* dpy);

// Get the raw socket fd (for custom poll loops).
int md_display_fd(md_display_t* dpy);

// ── Surfaces (windows) ───────────────────────────────────────────────────

// Create a new surface (window).  Returns NULL on failure.
md_client_surface_t* md_surface_create(md_display_t* dpy);

// Destroy a surface.
void md_surface_destroy(md_client_surface_t* surf);

// Attach a buffer to the surface (sets the content to display).
void md_surface_attach(md_client_surface_t* surf, md_client_buffer_t* buf);

// Mark a rectangular region as damaged (needs repainting).
void md_surface_damage(md_client_surface_t* surf,
                       int32_t x, int32_t y, uint32_t w, uint32_t h);

// Commit the current state (attached buffer + damage) to the compositor.
void md_surface_commit(md_client_surface_t* surf);

// Set the window title.
void md_surface_set_title(md_client_surface_t* surf, const char* title);

// Get the server-assigned surface ID.
uint32_t md_surface_id(md_client_surface_t* surf);

// Set user data pointer on a surface.
void  md_surface_set_userdata(md_client_surface_t* surf, void* data);
void* md_surface_get_userdata(md_client_surface_t* surf);

// ── Buffers ──────────────────────────────────────────────────────────────

// Create a pixel buffer backed by shared memory.
// Pixel format is MD_FORMAT_BGRX8888 (native).
// Returns NULL on failure.
md_client_buffer_t* md_buffer_create(md_display_t* dpy,
                                     uint32_t width, uint32_t height);

// Destroy a buffer and free its shared memory.
void md_buffer_destroy(md_client_buffer_t* buf);

// Get pointer to pixel data (BGRX, stride = width * 4).
uint32_t* md_buffer_data(md_client_buffer_t* buf);

// Get buffer dimensions.
uint32_t md_buffer_width(md_client_buffer_t* buf);
uint32_t md_buffer_height(md_client_buffer_t* buf);
uint32_t md_buffer_stride(md_client_buffer_t* buf);

// Set a callback for when the compositor releases a buffer.
void md_buffer_on_release(md_client_buffer_t* buf, md_buffer_release_handler_t handler);

// ── Resize helper ────────────────────────────────────────────────────────
//
// Atomically swap a surface's buffer for one of the requested new size:
//   1. Allocate a fresh buffer sized `width x height`.
//   2. Call `render(buf, userdata)` so the client can paint it (the render
//      callback is where the client decides whether to reflow, scale, or
//      re-render at the new native resolution).
//   3. Attach + full-surface damage + commit the new buffer.
//   4. Destroy the old buffer (if any).
//   5. Overwrite `*inout_buf` with the new buffer pointer.
//
// Returns 0 on success, -1 if buffer allocation failed (the old buffer is
// left untouched in that case).
//
// This is the standard path for MD_SURFACE_CONFIGURE handlers. Keeping the
// allocate/render/commit/destroy steps in one helper means clients never
// race with the compositor's "buffer still in use" state: the new buffer is
// already attached and committed before the old one is released.
typedef void (*md_resize_render_fn)(md_client_buffer_t* buf, void* userdata);

int md_surface_resize_commit(md_client_surface_t* surf,
                             md_client_buffer_t** inout_buf,
                             uint32_t width, uint32_t height,
                             md_resize_render_fn render, void* userdata);

// ── Double-buffering helper (md_dbuf) ────────────────────────────────────
//
// Wraps two md_client_buffers and tracks which is in-flight at the
// compositor. Typical render loop:
//
//     md_client_buffer_t* buf = md_dbuf_acquire(db);  // may block
//     paint(buf);
//     md_dbuf_present(db);
//
// acquire() blocks (dispatching events) until a buffer is free, so the
// client never stomps on a frame the compositor is still reading.
// present() attaches + damages the full surface + commits.

md_dbuf_t* md_dbuf_create(md_client_surface_t* surf,
                          uint32_t width, uint32_t height);
void       md_dbuf_destroy(md_dbuf_t* db);

// Destroy the old pair and allocate two new buffers at the given size.
// Returns 0 on success, -1 if allocation failed (db is left unusable).
int        md_dbuf_resize(md_dbuf_t* db, uint32_t width, uint32_t height);

// Block (dispatching events) until a back buffer is free, and return it.
// Returns NULL on connection loss.
md_client_buffer_t* md_dbuf_acquire(md_dbuf_t* db);

// Attach/damage/commit the buffer most recently returned by acquire().
void md_dbuf_present(md_dbuf_t* db);

uint32_t md_dbuf_width(md_dbuf_t* db);
uint32_t md_dbuf_height(md_dbuf_t* db);

// ── Event handler registration ───────────────────────────────────────────

void md_surface_on_key(md_client_surface_t* surf, md_key_handler_t handler);
void md_surface_on_configure(md_client_surface_t* surf, md_configure_handler_t handler);
void md_surface_on_close(md_client_surface_t* surf, md_close_handler_t handler);
void md_surface_on_focus(md_client_surface_t* surf, md_focus_handler_t handler);
void md_surface_on_pointer_move(md_client_surface_t* surf, md_pointer_move_handler_t handler);
void md_surface_on_pointer_button(md_client_surface_t* surf, md_pointer_button_handler_t handler);
