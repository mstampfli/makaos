// MakaDisplay Compositor — MakaOS display server
//
// Maps the physical framebuffer, listens on a Unix domain socket,
// and composites client surfaces.  Server-side window decorations.
// Reads keyboard from /dev/input/event0 and mouse from /dev/mouse.
//
// Single-threaded, poll-based main loop.

#include "libc.h"
#include "protocol.h"

// ── Forward declarations ─────────────────────────────────────────────────

typedef struct md_client  md_client_t;
typedef struct md_surface md_surface_t;
typedef struct md_buffer  md_buffer_t;

// ── Framebuffer state ────────────────────────────────────────────────────

static uint32_t* g_fb;           // mapped framebuffer (MMIO, uncached — slow!)
static uint32_t* g_bb;           // backbuffer in regular RAM (fast)
static uint32_t  g_fb_width;
static uint32_t  g_fb_height;
static uint32_t  g_fb_pitch;     // in bytes
static uint32_t  g_fb_stride;    // in pixels (pitch / 4)

// ── Input event structures (matches kernel) ─────────────────────────────

typedef struct {
    uint64_t time_ns;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input_event_t;

#define EV_SYN 0
#define EV_KEY 1

typedef struct {
    int16_t  dx;
    int16_t  dy;
    uint8_t  buttons;
} __attribute__((packed)) mouse_event_t;

#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

// ── Server-side buffer object ────────────────────────────────────────────

struct md_buffer {
    uint32_t    id;
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride;      // bytes per row
    uint32_t    format;
    uint32_t    offset;      // byte offset into shmem
    int         shm_fd;      // shmem fd (received via recvfd)
    uint8_t*    data;        // mmap'd pointer to pixel data
    uint32_t    data_size;   // mmap'd region size
    int         in_use;      // 1 if compositor is reading from it
    md_client_t* owner;
};

// ── Per-surface damage accumulator ───────────────────────────────────────
// Clients send MD_SURFACE_DAMAGE rects between attach and commit.  We merge
// them into a single bounding box per surface.  On commit, only that box
// (translated to screen coords) is added to the global dirty region.
// If no damage was reported, we conservatively dirty the whole surface.

#define SURFACE_DAMAGE_MAX 1  // we only track a merged bounding box

typedef struct {
    int32_t  x0, y0, x1, y1;  // accumulated bounding box (surface-local coords)
    int      has_damage;       // 0 = no damage rects received since last commit
} surface_damage_t;

// ── Server-side surface object ───────────────────────────────────────────

struct md_surface {
    uint32_t    id;
    int32_t     x, y;           // position on screen (client content origin)
    // Compositor-side "requested" size. This is what decorations, hit-testing,
    // and blit clipping use — NOT the size of the currently-attached buffer.
    // Set on first commit from the initial buffer, then only modified by
    // resize requests and maximize. The client's buffer may be larger or
    // smaller; the compositor clips to this rectangle regardless. That way a
    // misbehaving or slow client can never paint outside the window it was
    // granted.
    uint32_t    width, height;
    char        title[MD_TITLE_MAX + 1];
    md_buffer_t* attached;      // currently attached buffer
    md_buffer_t* prev_buffer;   // previously displayed buffer (for release events)
    int         committed;      // 1 if surface has been committed at least once
    int         focused;
    int         visible;
    int         moving;         // 1 during title-bar drag
    int32_t     drag_ox, drag_oy; // offset from surface origin to grab point
    md_client_t* owner;
    md_surface_t* z_prev;       // z-order doubly-linked list
    md_surface_t* z_next;
    surface_damage_t damage;    // accumulated damage since last commit
    // Maximize state: saved geometry for restore
    int         maximized;
    int32_t     saved_x, saved_y;
    uint32_t    saved_w, saved_h;
};

// ── Client connection ────────────────────────────────────────────────────

// Per-client outgoing buffer. Lazily allocated: a healthy client never
// fills its kernel socket buffer, so tx_buf stays NULL and costs nothing.
// On the first EAGAIN we malloc MD_TX_INITIAL bytes and double as needed
// up to MD_TX_MAX. If the cap would be exceeded, the client is dropped —
// a compositor must never stall on one misbehaving peer.
#define MD_TX_INITIAL     (4  * 1024)
#define MD_TX_MAX         (64 * 1024)

// ── Liveness detection tunables ──────────────────────────────────────────
// Every MD_PING_INTERVAL_MS a ping is sent to each client and the oldest
// outstanding serial is checked. If no pong is received within
// MD_PING_TIMEOUT_MS of the send, the client is flagged "not responding"
// — its title bar gets "(Not Responding)" appended and is dimmed, matching
// the Windows DWM ghost-window convention. The client is NEVER killed
// automatically; only a user-initiated click on the X of an unresponsive
// window escalates to SIGKILL. Matches Wayland/X11/Windows/macOS.
#define MD_PING_INTERVAL_MS  1000u
#define MD_PING_TIMEOUT_MS   5000u

struct md_client {
    int          fd;
    int          active;
    int          dead;       // marked for disconnect after current dispatch
    uint32_t     peer_pid;   // kernel-trusted pid of the connecting process
    uint32_t     next_id;    // next server-allocated (even) object id
    md_surface_t surfaces[MD_MAX_SURFACES];
    uint32_t     surface_count;
    md_buffer_t  buffers[MD_MAX_BUFFERS];
    uint32_t     buffer_count;
    uint32_t     seat_id;    // 0 if no seat allocated
    // Read buffer for partial message reads
    uint8_t      read_buf[MD_MAX_MSG_SIZE * 2];
    uint32_t     read_len;
    // Lazily-allocated outgoing queue: drained on POLLOUT. NULL when empty
    // — zero cost for healthy clients.
    uint8_t*     tx_buf;
    uint32_t     tx_len;
    uint32_t     tx_cap;
    // ── Liveness tracking ────────────────────────────────────────────────
    uint32_t     ping_serial;         // next serial to issue
    uint32_t     outstanding_serial;  // serial of earliest unanswered ping, 0 = none
    uint64_t     outstanding_since_ms; // ms timestamp of that ping
    uint64_t     last_ping_sent_ms;    // ms timestamp of last ping attempt
    int          unresponsive;         // 1 = past MD_PING_TIMEOUT_MS without pong
};

// ── Global state ─────────────────────────────────────────────────────────

static md_client_t  g_clients[MD_MAX_CLIENTS];
static uint32_t     g_client_count;
static int          g_listener_fd = -1;
static int          g_needs_recomposite;

// Z-order: bottom -> top linked list
static md_surface_t* g_z_bottom;
static md_surface_t* g_z_top;

// Focused surface (receives input)
static md_surface_t* g_focused;

// ── Dirty rectangle tracking ─────────────────────────────────────────────
static int32_t  g_dirty_x0, g_dirty_y0, g_dirty_x1, g_dirty_y1;
static int      g_dirty_full;

// Damage overlay debug: when enabled, the compositor tints the last dirty
// rectangle on every frame so you can see exactly what got recomposited.
// Toggle with F12.
static int      g_debug_damage;
static void dirty_reset(void);
static void dirty_add(int32_t x, int32_t y, uint32_t w, uint32_t h);
static void dirty_add_surface(md_surface_t* s);

// ── Mouse / cursor state ─────────────────────────────────────────────────

#define CURSOR_MAX_W 24
#define CURSOR_MAX_H 24

typedef struct {
    uint8_t w;
    uint8_t h;
    int8_t  hotx;
    int8_t  hoty;
    const uint8_t* data;
} cursor_shape_t;

static int32_t  g_cursor_x;
static int32_t  g_cursor_y;
static uint8_t  g_mouse_buttons;     // current button state
static uint8_t  g_mouse_buttons_prev; // previous button state

// Cursor save/restore — avoids full recomposite on every mouse move.
// g_cursor_shape is what we want to draw next; g_cursor_saved_shape is
// what was actually drawn on the backbuffer and must be used when erasing
// (a cursor shape change between erase and draw would otherwise leave
// pixels of the old shape on screen).
static uint32_t g_cursor_save[CURSOR_MAX_W * CURSOR_MAX_H];
static int32_t  g_cursor_save_x;  // top-left of save region
static int32_t  g_cursor_save_y;
static int       g_cursor_drawn;   // whether save buffer is valid
static const cursor_shape_t* g_cursor_shape;
static const cursor_shape_t* g_cursor_saved_shape;

// Dragging state
static md_surface_t* g_drag_surface;  // surface being dragged (title bar move)

// ── Rubber-band edge resize state ────────────────────────────────────────
// During a resize drag the surface is NOT mutated — we draw an outline over
// the composited frame as visual feedback. On mouse release the compositor
// updates s->width/s->height to the requested size and sends a single
// MD_SURFACE_CONFIGURE. The client then commits a new buffer on its own
// schedule; the compositor clips the buffer to the requested rect in the
// meantime, so stale pixels can never leak outside the window.
static md_surface_t* g_resize_surface;    // surface being resized, or NULL
static int      g_resize_edge;             // bitmask: 1=L 2=R 4=T 8=B
static int32_t  g_resize_start_x;          // surface geom at drag start
static int32_t  g_resize_start_y;
static uint32_t g_resize_start_w;
static uint32_t g_resize_start_h;
static int32_t  g_resize_anchor_x;         // cursor pos at drag start
static int32_t  g_resize_anchor_y;
static int32_t  g_resize_pending_x;        // proposed geom (current frame)
static int32_t  g_resize_pending_y;
static int32_t  g_resize_pending_w;
static int32_t  g_resize_pending_h;
static int32_t  g_resize_last_x;           // last proposed geom (prev frame)
static int32_t  g_resize_last_y;
static int32_t  g_resize_last_w;
static int32_t  g_resize_last_h;
static int      g_resize_has_last;

#define RESIZE_EDGE_GRAB 6     // px inside the border that count as an edge
#define MIN_SURFACE_W    80
#define MIN_SURFACE_H    40

// ── Modifier key tracking ────────────────────────────────────────────────

static uint32_t g_mod_state;  // MD_MOD_SHIFT | MD_MOD_CTRL | MD_MOD_ALT etc.

// Key codes (Linux evdev KEY_* codes)
#define KEY_LEFTSHIFT   42
#define KEY_RIGHTSHIFT  54
#define KEY_LEFTCTRL    29
#define KEY_RIGHTCTRL   97
#define KEY_LEFTALT     56
#define KEY_RIGHTALT    100
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F12         88

// ── Configure serial counter ─────────────────────────────────────────────

static uint32_t g_configure_serial;

// ── VT switching state ───────────────────────────────────────────────────
// Ctrl+Alt+F1..F6: F1 = display server (active), F2..F6 = reserved for VTs.
// When display server is "switched away", we suppress compositing and input
// routing.  For now, only F1 switches back to the display server.

static int g_vt_active = 1;  // 1 = display server is on-screen

// ── Backbuffer → framebuffer flip ────────────────────────────────────────
// Copies the entire backbuffer to the MMIO framebuffer in one pass.

static void fb_flip(void) {
    uint32_t* src = g_bb;
    uint32_t* dst = g_fb;
    uint64_t total = (uint64_t)g_fb_stride * g_fb_height;
    for (uint64_t i = 0; i < total; i++)
        dst[i] = src[i];
}

// Copies only a rectangular region from backbuffer to framebuffer.
static void fb_flip_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (x < 0) { if ((uint32_t)(-x) >= w) return; w += x; x = 0; }
    if (y < 0) { if ((uint32_t)(-y) >= h) return; h += y; y = 0; }
    if ((uint32_t)x >= g_fb_width || (uint32_t)y >= g_fb_height) return;
    if ((uint32_t)x + w > g_fb_width)  w = g_fb_width - (uint32_t)x;
    if ((uint32_t)y + h > g_fb_height) h = g_fb_height - (uint32_t)y;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t off = ((uint32_t)y + row) * g_fb_stride + (uint32_t)x;
        for (uint32_t col = 0; col < w; col++)
            g_fb[off + col] = g_bb[off + col];
    }
}

// ── Helper: fill a rectangle in the backbuffer ──────────────────────────

static void fb_fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x < 0) { if ((uint32_t)(-x) >= w) return; w += x; x = 0; }
    if (y < 0) { if ((uint32_t)(-y) >= h) return; h += y; y = 0; }
    if ((uint32_t)x >= g_fb_width || (uint32_t)y >= g_fb_height) return;
    if ((uint32_t)x + w > g_fb_width)  w = g_fb_width - (uint32_t)x;
    if ((uint32_t)y + h > g_fb_height) h = g_fb_height - (uint32_t)y;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t* dst = g_bb + ((uint32_t)y + row) * g_fb_stride + (uint32_t)x;
        for (uint32_t col = 0; col < w; col++)
            dst[col] = color;
    }
}

// Draw a rectangle outline (unfilled) of the given thickness. Used by the
// rubber-band resize preview. All 4 strips are clipped against the fb.
static void fb_draw_outline(int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t thick, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    uint32_t uw = (uint32_t)w, uh = (uint32_t)h;
    fb_fill_rect(x, y, uw, thick, color);                           // top
    if (uh > thick)
        fb_fill_rect(x, y + (int32_t)(uh - thick), uw, thick, color); // bottom
    fb_fill_rect(x, y, thick, uh, color);                           // left
    if (uw > thick)
        fb_fill_rect(x + (int32_t)(uw - thick), y, thick, uh, color); // right
}

// ── Helper: blit a client buffer region to framebuffer ───────────────────

static void fb_blit_buffer(md_buffer_t* buf, int32_t dst_x, int32_t dst_y,
                           uint32_t src_x, uint32_t src_y,
                           uint32_t w, uint32_t h) {
    if (!buf || !buf->data) return;

    // Clip to framebuffer
    if (dst_x < 0) { int32_t clip = -dst_x; src_x += clip; if (clip >= (int32_t)w) return; w -= clip; dst_x = 0; }
    if (dst_y < 0) { int32_t clip = -dst_y; src_y += clip; if (clip >= (int32_t)h) return; h -= clip; dst_y = 0; }
    if ((uint32_t)dst_x >= g_fb_width || (uint32_t)dst_y >= g_fb_height) return;
    if ((uint32_t)dst_x + w > g_fb_width)  w = g_fb_width - (uint32_t)dst_x;
    if ((uint32_t)dst_y + h > g_fb_height) h = g_fb_height - (uint32_t)dst_y;

    // Clip to buffer
    if (src_x >= buf->width || src_y >= buf->height) return;
    if (src_x + w > buf->width)  w = buf->width - src_x;
    if (src_y + h > buf->height) h = buf->height - src_y;

    uint32_t buf_stride_px = buf->stride / 4;
    uint32_t* src_base = (uint32_t*)(buf->data + buf->offset);

    for (uint32_t row = 0; row < h; row++) {
        uint32_t* src = src_base + (src_y + row) * buf_stride_px + src_x;
        uint32_t* dst = g_bb + ((uint32_t)dst_y + row) * g_fb_stride + (uint32_t)dst_x;
        for (uint32_t col = 0; col < w; col++)
            dst[col] = src[col];
    }
}

// ── Z-order management ───────────────────────────────────────────────────

static void z_remove(md_surface_t* s) {
    if (s->z_prev) s->z_prev->z_next = s->z_next;
    else g_z_bottom = s->z_next;
    if (s->z_next) s->z_next->z_prev = s->z_prev;
    else g_z_top = s->z_prev;
    s->z_prev = s->z_next = 0;
}

static void z_push_top(md_surface_t* s) {
    s->z_prev = g_z_top;
    s->z_next = 0;
    if (g_z_top) g_z_top->z_next = s;
    else g_z_bottom = s;
    g_z_top = s;
}

// ── Focus management ─────────────────────────────────────────────────────

static void send_msg(md_client_t* c, const void* msg, uint32_t size);
static void client_tx_flush(md_client_t* c);

static void focus_surface(md_surface_t* s) {
    if (g_focused == s) return;

    // Send leave to old focused surface
    if (g_focused && g_focused->owner && g_focused->owner->active) {
        g_focused->focused = 0;
        md_msg_header_t leave = md_msg(g_focused->id, MD_SURFACE_LEAVE, MD_HEADER_SIZE);
        send_msg(g_focused->owner, &leave, MD_HEADER_SIZE);
    }

    g_focused = s;

    if (s) {
        s->focused = 1;
        // Raise to top of z-order
        z_remove(s);
        z_push_top(s);
        // Send enter event
        if (s->owner && s->owner->active) {
            md_msg_header_t enter = md_msg(s->id, MD_SURFACE_ENTER, MD_HEADER_SIZE);
            send_msg(s->owner, &enter, MD_HEADER_SIZE);
        }
    }
    g_dirty_full = 1;
    g_needs_recomposite = 1;
}

// ── Message sending ──────────────────────────────────────────────────────
//
// Nonblocking fan-out: every per-client send is attempted directly; if the
// peer's recv buffer is full (EAGAIN) or there is already queued data, the
// message is appended to the client's tx_buf and flushed later on POLLOUT.
// If tx_buf would overflow MD_CLIENT_TX_CAP, the client is marked dead —
// one stuck client can never block the entire compositor.

// Grow c->tx_buf so it can hold at least `need` total bytes. Starts at
// MD_TX_INITIAL on first use, doubles from there, and refuses to grow past
// MD_TX_MAX. Returns 0 on success, -1 if the cap would be exceeded.
static int client_tx_reserve(md_client_t* c, uint32_t need) {
    if (need <= c->tx_cap) return 0;
    if (need > MD_TX_MAX)  return -1;

    uint32_t new_cap = c->tx_cap ? c->tx_cap : MD_TX_INITIAL;
    while (new_cap < need) new_cap *= 2;
    if (new_cap > MD_TX_MAX) new_cap = MD_TX_MAX;

    uint8_t* nb = (uint8_t*)malloc(new_cap);
    if (!nb) return -1;
    for (uint32_t i = 0; i < c->tx_len; i++) nb[i] = c->tx_buf[i];
    if (c->tx_buf) free(c->tx_buf);
    c->tx_buf = nb;
    c->tx_cap = new_cap;
    return 0;
}

static void client_tx_append(md_client_t* c, const uint8_t* p, uint32_t n) {
    if (client_tx_reserve(c, c->tx_len + n) < 0) {
        c->dead = 1;
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        c->tx_buf[c->tx_len + i] = p[i];
    c->tx_len += n;
}

static void client_tx_flush(md_client_t* c) {
    if (!c->active || c->dead || c->tx_len == 0) return;
    uint32_t off = 0;
    while (off < c->tx_len) {
        int r = (int)send(c->fd, c->tx_buf + off, c->tx_len - off, 0);
        if (r > 0) { off += (uint32_t)r; continue; }
        if (r < 0 && errno == EAGAIN) break;   // still full, wait for next POLLOUT
        c->dead = 1;                            // EPIPE / ECONNRESET / etc.
        return;
    }
    // Shift remaining bytes to the front.
    if (off > 0) {
        uint32_t rem = c->tx_len - off;
        for (uint32_t i = 0; i < rem; i++)
            c->tx_buf[i] = c->tx_buf[off + i];
        c->tx_len = rem;
    }
}

static void send_msg(md_client_t* c, const void* msg, uint32_t size) {
    if (!c || !c->active || c->dead) return;

    const uint8_t* p = (const uint8_t*)msg;

    // If there's already queued data, we must preserve ordering: append
    // rather than try to send now.
    if (c->tx_len > 0) {
        client_tx_append(c, p, size);
        return;
    }

    uint32_t sent = 0;
    while (sent < size) {
        int r = (int)send(c->fd, p + sent, size - sent, 0);
        if (r > 0) { sent += (uint32_t)r; continue; }
        if (r < 0 && errno == EAGAIN) break;   // peer full — queue the rest
        c->dead = 1;                            // peer closed or error
        return;
    }
    if (sent < size)
        client_tx_append(c, p + sent, size - sent);
}

// ── Client management ────────────────────────────────────────────────────

static md_client_t* client_alloc(int fd) {
    for (uint32_t i = 0; i < MD_MAX_CLIENTS; i++) {
        if (!g_clients[i].active) {
            md_client_t* c = &g_clients[i];
            for (uint32_t j = 0; j < sizeof(md_client_t); j++)
                ((uint8_t*)c)[j] = 0;
            c->fd = fd;
            c->active = 1;
            c->next_id = 2;  // even IDs start at 2
            g_client_count++;
            return c;
        }
    }
    return 0;
}

static void surface_destroy(md_surface_t* s);

static void client_disconnect(md_client_t* c) {
    if (!c->active) return;

    // Destroy all surfaces
    for (uint32_t i = 0; i < MD_MAX_SURFACES; i++) {
        if (c->surfaces[i].id)
            surface_destroy(&c->surfaces[i]);
    }

    // Close/unmap all buffers
    for (uint32_t i = 0; i < MD_MAX_BUFFERS; i++) {
        if (c->buffers[i].id) {
            if (c->buffers[i].data) {
                munmap(c->buffers[i].data, c->buffers[i].data_size);
                c->buffers[i].data = 0;
            }
            if (c->buffers[i].shm_fd >= 0)
                close(c->buffers[i].shm_fd);
            c->buffers[i].id = 0;
        }
    }

    close(c->fd);
    if (c->tx_buf) { free(c->tx_buf); c->tx_buf = 0; }
    c->tx_len = 0;
    c->tx_cap = 0;
    c->active = 0;
    c->dead   = 0;
    c->fd = -1;
    g_client_count--;
    g_dirty_full = 1;
    g_needs_recomposite = 1;
}

// ── Surface management ───────────────────────────────────────────────────

static md_surface_t* surface_create(md_client_t* c) {
    if (c->surface_count >= MD_MAX_SURFACES) return 0;

    for (uint32_t i = 0; i < MD_MAX_SURFACES; i++) {
        if (!c->surfaces[i].id) {
            md_surface_t* s = &c->surfaces[i];
            for (uint32_t j = 0; j < sizeof(md_surface_t); j++)
                ((uint8_t*)s)[j] = 0;
            s->id = c->next_id;
            c->next_id += 2;
            s->x = 50 + (int32_t)(c->surface_count * 30);
            s->y = 80 + (int32_t)(c->surface_count * 30);
            s->width = 400;
            s->height = 300;
            s->visible = 0;  // invisible until first commit
            s->owner = c;
            s->title[0] = 'W'; s->title[1] = 'i'; s->title[2] = 'n';
            s->title[3] = 'd'; s->title[4] = 'o'; s->title[5] = 'w';
            s->title[6] = '\0';
            c->surface_count++;
            z_push_top(s);
            // Don't focus until first commit — the surface has no content yet,
            // focusing it would steal input from the currently active window.
            return s;
        }
    }
    return 0;
}

static void surface_send_configure(md_surface_t* s, uint32_t states) {
    if (!s || !s->owner) return;
    md_surface_configure_t cfg;
    cfg.hdr = md_msg(s->id, MD_SURFACE_CONFIGURE, sizeof(cfg));
    cfg.width  = s->width;
    cfg.height = s->height;
    cfg.states = states;
    cfg.serial = ++g_configure_serial;
    send_msg(s->owner, &cfg, sizeof(cfg));
}

static void surface_toggle_maximize(md_surface_t* s) {
    if (!s || !s->id || !s->owner) return;

    dirty_add_surface(s);  // dirty old position

    if (s->maximized) {
        // Restore to saved geometry.
        s->x = s->saved_x;
        s->y = s->saved_y;
        s->width = s->saved_w;
        s->height = s->saved_h;
        s->maximized = 0;
    } else {
        // Save current geometry, then maximize.
        s->saved_x = s->x;
        s->saved_y = s->y;
        s->saved_w = s->width;
        s->saved_h = s->height;
        // Maximized: fill screen under title bar + borders.
        s->x = MD_DECO_BORDER_W;
        s->y = MD_DECO_TITLEBAR_H + MD_DECO_BORDER_W;
        s->width = g_fb_width - 2 * MD_DECO_BORDER_W;
        s->height = g_fb_height - MD_DECO_TITLEBAR_H - 2 * MD_DECO_BORDER_W;
        s->maximized = 1;
    }

    // Tell the client the new size so it can reallocate its buffer.
    uint32_t states = s->focused ? MD_SURFACE_STATE_FOCUSED : 0;
    if (s->maximized) states |= MD_SURFACE_STATE_MAXIMIZED;
    surface_send_configure(s, states);

    dirty_add_surface(s);  // dirty new position
    g_dirty_full = 1;      // safe: window size changed
    g_needs_recomposite = 1;
}

static void surface_destroy(md_surface_t* s) {
    if (!s || !s->id) return;
    int was_focused = (g_focused == s);
    if (g_focused == s) g_focused = 0;
    if (g_drag_surface == s) g_drag_surface = 0;
    if (g_resize_surface == s) { g_resize_surface = 0; g_resize_has_last = 0; }
    z_remove(s);
    s->id = 0;
    s->attached = 0;
    s->prev_buffer = 0;
    s->committed = 0;
    s->visible = 0;
    if (s->owner) s->owner->surface_count--;
    g_dirty_full = 1;
    g_needs_recomposite = 1;
    // If the destroyed surface had focus, pass focus to the new top window.
    if (was_focused && g_z_top) {
        focus_surface(g_z_top);
    }
}

// ── Buffer management ────────────────────────────────────────────────────

static md_buffer_t* buffer_create(md_client_t* c, md_buffer_create_t* msg) {
    if (c->buffer_count >= MD_MAX_BUFFERS) return 0;

    for (uint32_t i = 0; i < MD_MAX_BUFFERS; i++) {
        if (!c->buffers[i].id) {
            md_buffer_t* buf = &c->buffers[i];
            for (uint32_t j = 0; j < sizeof(md_buffer_t); j++)
                ((uint8_t*)buf)[j] = 0;
            buf->id = c->next_id;
            c->next_id += 2;
            buf->width  = msg->width;
            buf->height = msg->height;
            buf->stride = msg->stride;
            buf->format = msg->format;
            buf->offset = msg->offset;
            buf->shm_fd = -1;
            buf->owner  = c;
            c->buffer_count++;
            return buf;
        }
    }
    return 0;
}

static md_surface_t* find_surface(md_client_t* c, uint32_t id) {
    for (uint32_t i = 0; i < MD_MAX_SURFACES; i++)
        if (c->surfaces[i].id == id) return &c->surfaces[i];
    return 0;
}

static md_buffer_t* find_buffer(md_client_t* c, uint32_t id) {
    for (uint32_t i = 0; i < MD_MAX_BUFFERS; i++)
        if (c->buffers[i].id == id) return &c->buffers[i];
    return 0;
}

// ── Protocol message handlers ────────────────────────────────────────────

static void handle_display_msg(md_client_t* c, md_msg_header_t* hdr) {
    switch (hdr->opcode) {
    case MD_DISPLAY_GET_SURFACE: {
        md_surface_t* s = surface_create(c);
        if (!s) return;
        md_display_object_id_t resp;
        resp.hdr = md_msg(0, MD_DISPLAY_OBJECT_ID, sizeof(resp));
        resp.new_id = s->id;
        resp.obj_type = MD_OBJ_SURFACE;
        send_msg(c, &resp, sizeof(resp));
        // No initial configure — the client picks its first buffer size and
        // the compositor adopts whatever dims the first commit supplies.
        // Configure events are only sent when the compositor initiates a
        // resize (edge drag, maximize, etc.).
        break;
    }
    case MD_DISPLAY_GET_SEAT: {
        if (!c->seat_id) {
            c->seat_id = c->next_id;
            c->next_id += 2;
        }
        md_display_object_id_t resp;
        resp.hdr = md_msg(0, MD_DISPLAY_OBJECT_ID, sizeof(resp));
        resp.new_id = c->seat_id;
        resp.obj_type = MD_OBJ_SEAT;
        send_msg(c, &resp, sizeof(resp));
        break;
    }
    case MD_DISPLAY_SYNC: {
        md_msg_header_t done = md_msg(0, MD_DISPLAY_DONE, MD_HEADER_SIZE);
        send_msg(c, &done, MD_HEADER_SIZE);
        break;
    }
    case MD_DISPLAY_PONG: {
        // Liveness reply. If the serial matches our outstanding ping, the
        // client pumped its event loop between our send and this receive —
        // it is (for now) responsive. Stale serials (e.g. a pong from before
        // we bumped the counter) are silently ignored.
        if (hdr->size < sizeof(md_display_ping_t)) return;
        md_display_ping_t* msg = (md_display_ping_t*)hdr;
        if (c->outstanding_serial && msg->serial == c->outstanding_serial) {
            c->outstanding_serial = 0;
            if (c->unresponsive) {
                c->unresponsive = 0;
                g_dirty_full = 1;
                g_needs_recomposite = 1;
            }
        }
        break;
    }
    case MD_DISPLAY_CREATE_BUFFER: {
        if (hdr->size < sizeof(md_buffer_create_t)) return;
        md_buffer_create_t* msg = (md_buffer_create_t*)hdr;

        md_buffer_t* buf = buffer_create(c, msg);
        if (!buf) return;

        // Receive the shmem fd via SCM_RIGHTS.
        // The client calls sendfd() right after sending CREATE_BUFFER,
        // so the fd should already be queued by the time we get here.
        int shm_fd = recvfd(c->fd);
        if (shm_fd < 0) {
            buf->id = 0;
            c->buffer_count--;
            return;
        }
        buf->shm_fd = shm_fd;

        // mmap the shmem
        uint32_t total_size = buf->offset + buf->stride * buf->height;
        void* data = mmap(0, total_size,
                          PROT_READ, MAP_SHARED, shm_fd, 0);
        if (data == (void*)-1L) {
            close(shm_fd);
            buf->shm_fd = -1;
            buf->id = 0;
            c->buffer_count--;
            return;
        }
        buf->data = (uint8_t*)data;
        buf->data_size = total_size;

        // Reply with the assigned buffer ID
        md_display_object_id_t resp;
        resp.hdr = md_msg(0, MD_DISPLAY_OBJECT_ID, sizeof(resp));
        resp.new_id = buf->id;
        resp.obj_type = MD_OBJ_BUFFER;
        send_msg(c, &resp, sizeof(resp));
        break;
    }
    }
}

static void handle_surface_msg(md_client_t* c, md_msg_header_t* hdr, uint8_t* payload) {
    md_surface_t* s = find_surface(c, hdr->object_id);
    if (!s) return;

    switch (hdr->opcode) {
    case MD_SURFACE_ATTACH: {
        if (hdr->size < sizeof(md_surface_attach_t)) return;
        md_surface_attach_t* msg = (md_surface_attach_t*)hdr;
        md_buffer_t* buf = find_buffer(c, msg->buffer_id);
        s->attached = buf;
        break;
    }
    case MD_SURFACE_DAMAGE: {
        if (hdr->size < sizeof(md_surface_damage_t)) break;
        md_surface_damage_t* dmg = (md_surface_damage_t*)hdr;
        // Merge into the surface's damage bounding box.
        int32_t dx0 = dmg->x;
        int32_t dy0 = dmg->y;
        int32_t dx1 = dmg->x + (int32_t)dmg->width;
        int32_t dy1 = dmg->y + (int32_t)dmg->height;
        if (!s->damage.has_damage) {
            s->damage.x0 = dx0;
            s->damage.y0 = dy0;
            s->damage.x1 = dx1;
            s->damage.y1 = dy1;
            s->damage.has_damage = 1;
        } else {
            if (dx0 < s->damage.x0) s->damage.x0 = dx0;
            if (dy0 < s->damage.y0) s->damage.y0 = dy0;
            if (dx1 > s->damage.x1) s->damage.x1 = dx1;
            if (dy1 > s->damage.y1) s->damage.y1 = dy1;
        }
        break;
    }
    case MD_SURFACE_COMMIT:
        if (s->attached) {
            // Release the previous buffer if it's different from the new one
            md_buffer_t* prev = s->prev_buffer;
            s->prev_buffer = s->attached;
            if (prev && prev != s->attached && prev->id && prev->owner) {
                md_msg_header_t rel;
                rel.object_id = prev->id;
                rel.opcode    = MD_BUFFER_RELEASE;
                rel.size      = MD_HEADER_SIZE;
                send_msg(prev->owner, &rel, MD_HEADER_SIZE);
            }
            int first_commit = !s->committed;
            // First commit adopts the buffer's size as the initial window
            // size. Subsequent commits do NOT change s->width/height — only
            // resize and maximize do. This keeps the compositor in control
            // of the window's apparent size even if the client commits a
            // buffer at a different size than requested (e.g. a fixed-size
            // app that ignores configure, or a slow client whose previous
            // buffer is still in flight during a resize).
            if (first_commit) {
                s->width  = s->attached->width;
                s->height = s->attached->height;
            }
            s->committed = 1;
            int size_changed = first_commit;
            // First commit: make visible and focus
            if (first_commit) {
                s->visible = 1;
                focus_surface(s);
            }
            // Use per-surface damage to dirty only the changed region.
            // If client sent damage rects, translate to screen coords.
            // If no damage or size changed, dirty the whole surface.
            if (s->damage.has_damage && !first_commit && !size_changed) {
                // Clamp damage to surface bounds.
                int32_t cx0 = s->damage.x0 < 0 ? 0 : s->damage.x0;
                int32_t cy0 = s->damage.y0 < 0 ? 0 : s->damage.y0;
                int32_t cx1 = s->damage.x1 > (int32_t)s->width  ? (int32_t)s->width  : s->damage.x1;
                int32_t cy1 = s->damage.y1 > (int32_t)s->height ? (int32_t)s->height : s->damage.y1;
                if (cx1 > cx0 && cy1 > cy0) {
                    dirty_add(s->x + cx0, s->y + cy0,
                              (uint32_t)(cx1 - cx0), (uint32_t)(cy1 - cy0));
                }
            } else {
                dirty_add_surface(s);
            }
            // Reset surface damage for next frame.
            s->damage.has_damage = 0;
            g_needs_recomposite = 1;
        }
        break;
    case MD_SURFACE_SET_TITLE: {
        if (hdr->size < sizeof(md_surface_set_title_t)) return;
        md_surface_set_title_t* msg = (md_surface_set_title_t*)hdr;
        uint32_t len = msg->len;
        if (len > MD_TITLE_MAX) len = MD_TITLE_MAX;
        for (uint32_t i = 0; i < len; i++)
            s->title[i] = msg->title[i];
        s->title[len] = '\0';
        dirty_add_surface(s);
        g_needs_recomposite = 1;
        break;
    }
    case MD_SURFACE_SET_POSITION: {
        if (hdr->size < sizeof(md_surface_set_position_t)) return;
        md_surface_set_position_t* msg = (md_surface_set_position_t*)hdr;
        dirty_add_surface(s);  // old position
        s->x = msg->x;
        s->y = msg->y;
        dirty_add_surface(s);  // new position
        g_needs_recomposite = 1;
        break;
    }
    case MD_SURFACE_DESTROY:
        surface_destroy(s);
        break;
    }
}

static void handle_buffer_msg(md_client_t* c, md_msg_header_t* hdr) {
    switch (hdr->opcode) {
    case MD_BUFFER_DESTROY: {
        md_buffer_t* buf = find_buffer(c, hdr->object_id);
        if (!buf) return;
        // Detach this buffer from any surface currently referencing it,
        // otherwise the next composite pass dereferences freed memory.
        // Also clear prev_buffer — the next commit's release-event logic
        // would otherwise dereference a freed slot that may have been
        // recycled by a subsequent buffer_create.
        for (uint32_t i = 0; i < MD_MAX_SURFACES; i++) {
            md_surface_t* s = &c->surfaces[i];
            if (!s->id) continue;
            if (s->attached == buf) {
                dirty_add_surface(s);
                s->attached = 0;
                s->committed = 0;
                s->visible = 0;
                g_needs_recomposite = 1;
            }
            if (s->prev_buffer == buf) {
                s->prev_buffer = 0;
            }
        }
        if (buf->data) {
            munmap(buf->data, buf->data_size);
            buf->data = 0;
        }
        if (buf->shm_fd >= 0) close(buf->shm_fd);
        buf->id = 0;
        c->buffer_count--;
        break;
    }
    }
}

// ── Dispatch incoming client message ─────────────────────────────────────

static void dispatch_message(md_client_t* c, uint8_t* msg, uint32_t len) {
    if (len < MD_HEADER_SIZE) return;
    md_msg_header_t* hdr = (md_msg_header_t*)msg;
    if (hdr->size > len) return;

    if (hdr->object_id == 0) {
        handle_display_msg(c, hdr);
        return;
    }

    if (find_surface(c, hdr->object_id)) {
        handle_surface_msg(c, hdr, msg + MD_HEADER_SIZE);
        return;
    }
    if (find_buffer(c, hdr->object_id)) {
        handle_buffer_msg(c, hdr);
        return;
    }
}

// ── Read and process messages from a client ──────────────────────────────

static void client_read(md_client_t* c) {
    uint32_t space = sizeof(c->read_buf) - c->read_len;
    if (space == 0) { c->read_len = 0; return; }

    int n = (int)recv(c->fd, c->read_buf + c->read_len, space, 0);
    if (n <= 0) {
        client_disconnect(c);
        return;
    }
    c->read_len += (uint32_t)n;

    uint32_t pos = 0;
    while (pos + MD_HEADER_SIZE <= c->read_len) {
        md_msg_header_t* hdr = (md_msg_header_t*)(c->read_buf + pos);
        if (hdr->size < MD_HEADER_SIZE || hdr->size > MD_MAX_MSG_SIZE) {
            client_disconnect(c);
            return;
        }
        if (pos + hdr->size > c->read_len) break;

        dispatch_message(c, c->read_buf + pos, hdr->size);
        pos += hdr->size;
    }

    if (pos > 0) {
        uint32_t remaining = c->read_len - pos;
        for (uint32_t i = 0; i < remaining; i++)
            c->read_buf[i] = c->read_buf[pos + i];
        c->read_len = remaining;
    }
}

// ── Compositing ──────────────────────────────────────────────────────────

static void draw_decoration(md_surface_t* s) {
    // Unresponsive clients get a "ghosted" title bar — desaturated/darker
    // than the normal focused/inactive colors. Matches Windows DWM ghost
    // windows. A future font-rendering pass will also append
    // " (Not Responding)" to the title text.
    int ghost = s->owner && s->owner->unresponsive;
    uint32_t title_color;
    if (ghost) {
        title_color = 0x00303840;   // dim blue-grey, clearly different
    } else {
        title_color = s->focused
            ? MD_COLOR_TITLEBAR_ACTIVE
            : MD_COLOR_TITLEBAR_INACTIVE;
    }

    int32_t bx = s->x - MD_DECO_BORDER_W;
    int32_t by = s->y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W;
    uint32_t total_w = s->width + 2 * MD_DECO_BORDER_W;
    uint32_t total_h = s->height + MD_DECO_TITLEBAR_H + 2 * MD_DECO_BORDER_W;

    // Border
    fb_fill_rect(bx, by, total_w, MD_DECO_BORDER_W, MD_COLOR_BORDER);
    fb_fill_rect(bx, by + total_h - MD_DECO_BORDER_W, total_w, MD_DECO_BORDER_W, MD_COLOR_BORDER);
    fb_fill_rect(bx, by, MD_DECO_BORDER_W, total_h, MD_COLOR_BORDER);
    fb_fill_rect(bx + total_w - MD_DECO_BORDER_W, by, MD_DECO_BORDER_W, total_h, MD_COLOR_BORDER);

    // Title bar background
    fb_fill_rect(s->x, s->y - MD_DECO_TITLEBAR_H,
                 s->width, MD_DECO_TITLEBAR_H, title_color);

    // Close button (red square in top-right of title bar)
    int32_t close_x = s->x + (int32_t)s->width - MD_DECO_BUTTON_W - 2;
    int32_t close_y = s->y - MD_DECO_TITLEBAR_H + 2;
    fb_fill_rect(close_x, close_y, MD_DECO_BUTTON_W, MD_DECO_BUTTON_H, MD_COLOR_CLOSE_BTN);

    // Draw an "X" in the close button
    for (int i = 4; i < (int)MD_DECO_BUTTON_W - 4; i++) {
        int32_t px1 = close_x + i;
        int32_t py1 = close_y + i;
        int32_t py2 = close_y + (int32_t)MD_DECO_BUTTON_H - 1 - i;
        if (px1 >= 0 && (uint32_t)px1 < g_fb_width) {
            if (py1 >= 0 && (uint32_t)py1 < g_fb_height)
                g_bb[(uint32_t)py1 * g_fb_stride + (uint32_t)px1] = MD_COLOR_TITLE_TEXT;
            if (py2 >= 0 && (uint32_t)py2 < g_fb_height)
                g_bb[(uint32_t)py2 * g_fb_stride + (uint32_t)px1] = MD_COLOR_TITLE_TEXT;
        }
    }

    // Maximize button (green square, to the left of close button)
    int32_t max_x = close_x - MD_DECO_BUTTON_W - 2;
    int32_t max_y = close_y;
    fb_fill_rect(max_x, max_y, MD_DECO_BUTTON_W, MD_DECO_BUTTON_H, MD_COLOR_MAXIMIZE_BTN);

    // Draw a rectangle outline (□) in the maximize button
    for (int i = 4; i < (int)MD_DECO_BUTTON_W - 4; i++) {
        int32_t px = max_x + i;
        if (px >= 0 && (uint32_t)px < g_fb_width) {
            int32_t py_top = max_y + 4;
            int32_t py_bot = max_y + (int32_t)MD_DECO_BUTTON_H - 5;
            if (py_top >= 0 && (uint32_t)py_top < g_fb_height)
                g_bb[(uint32_t)py_top * g_fb_stride + (uint32_t)px] = MD_COLOR_TITLE_TEXT;
            if (py_bot >= 0 && (uint32_t)py_bot < g_fb_height)
                g_bb[(uint32_t)py_bot * g_fb_stride + (uint32_t)px] = MD_COLOR_TITLE_TEXT;
        }
    }
    for (int j = 4; j < (int)MD_DECO_BUTTON_H - 4; j++) {
        int32_t py = max_y + j;
        if (py >= 0 && (uint32_t)py < g_fb_height) {
            int32_t px_left = max_x + 4;
            int32_t px_right = max_x + (int32_t)MD_DECO_BUTTON_W - 5;
            if (px_left >= 0 && (uint32_t)px_left < g_fb_width)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px_left] = MD_COLOR_TITLE_TEXT;
            if (px_right >= 0 && (uint32_t)px_right < g_fb_width)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px_right] = MD_COLOR_TITLE_TEXT;
        }
    }
}

// ── Cursor rendering ─────────────────────────────────────────────────────
// Pixel values: 0 = transparent, 1 = white fill, 2 = black outline.

static const uint8_t g_cursor_arrow[19 * 12] = {
    2,0,0,0,0,0,0,0,0,0,0,0,
    2,2,0,0,0,0,0,0,0,0,0,0,
    2,1,2,0,0,0,0,0,0,0,0,0,
    2,1,1,2,0,0,0,0,0,0,0,0,
    2,1,1,1,2,0,0,0,0,0,0,0,
    2,1,1,1,1,2,0,0,0,0,0,0,
    2,1,1,1,1,1,2,0,0,0,0,0,
    2,1,1,1,1,1,1,2,0,0,0,0,
    2,1,1,1,1,1,1,1,2,0,0,0,
    2,1,1,1,1,1,1,1,1,2,0,0,
    2,1,1,1,1,1,1,1,1,1,2,0,
    2,1,1,1,1,1,1,2,2,2,2,0,
    2,1,1,1,2,1,1,2,0,0,0,0,
    2,1,1,2,0,2,1,1,2,0,0,0,
    2,1,2,0,0,2,1,1,2,0,0,0,
    2,2,0,0,0,0,2,1,1,2,0,0,
    2,0,0,0,0,0,2,1,1,2,0,0,
    0,0,0,0,0,0,0,2,1,2,0,0,
    0,0,0,0,0,0,0,0,2,0,0,0,
};

// 15x9 double-headed horizontal arrow, hotspot (7,4)
static const uint8_t g_cursor_hresize[9 * 15] = {
    0,0,0,0,2,0,0,0,0,0,2,0,0,0,0,
    0,0,0,2,2,0,0,0,0,0,2,2,0,0,0,
    0,0,2,1,2,2,2,2,2,2,2,1,2,0,0,
    0,2,1,1,1,1,1,1,1,1,1,1,1,2,0,
    2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,
    0,2,1,1,1,1,1,1,1,1,1,1,1,2,0,
    0,0,2,1,2,2,2,2,2,2,2,1,2,0,0,
    0,0,0,2,2,0,0,0,0,0,2,2,0,0,0,
    0,0,0,0,2,0,0,0,0,0,2,0,0,0,0,
};

// 9x15 double-headed vertical arrow, hotspot (4,7)
static const uint8_t g_cursor_vresize[15 * 9] = {
    0,0,0,0,2,0,0,0,0,
    0,0,0,2,1,2,0,0,0,
    0,0,2,1,1,1,2,0,0,
    0,2,1,1,1,1,1,2,0,
    2,2,2,2,1,2,2,2,2,
    0,0,0,2,1,2,0,0,0,
    0,0,0,2,1,2,0,0,0,
    0,0,0,2,1,2,0,0,0,
    0,0,0,2,1,2,0,0,0,
    0,0,0,2,1,2,0,0,0,
    2,2,2,2,1,2,2,2,2,
    0,2,1,1,1,1,1,2,0,
    0,0,2,1,1,1,2,0,0,
    0,0,0,2,1,2,0,0,0,
    0,0,0,0,2,0,0,0,0,
};

// 15x15 diagonal (NW-SE) double arrow, hotspot (7,7)
static const uint8_t g_cursor_nwse[15 * 15] = {
    2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,2,0,0,0,0,0,0,0,0,0,
    2,1,1,1,2,2,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,2,0,0,0,0,0,0,0,0,0,
    2,1,2,1,1,1,2,0,0,0,0,0,0,0,0,
    2,2,2,2,1,1,1,2,0,0,0,0,0,0,0,
    0,0,0,2,2,1,1,1,2,0,0,0,2,2,2,
    0,0,0,0,0,2,1,1,1,2,0,2,1,1,2,
    0,0,0,0,0,0,2,1,1,1,2,1,1,2,0,
    0,0,0,0,0,0,0,2,1,1,1,1,1,2,0,
    0,0,0,0,0,0,0,0,2,1,1,1,2,1,2,
    0,0,0,0,0,0,0,0,0,2,1,1,1,1,2,
    0,0,0,0,0,0,0,0,0,2,2,1,1,1,2,
    0,0,0,0,0,0,0,0,0,0,0,2,1,1,2,
    0,0,0,0,0,0,0,0,0,0,0,2,2,2,2,
};

// 15x15 diagonal (NE-SW) double arrow, hotspot (7,7)
static const uint8_t g_cursor_nesw[15 * 15] = {
    0,0,0,0,0,0,0,0,0,2,2,2,2,2,2,
    0,0,0,0,0,0,0,0,0,2,1,1,1,1,2,
    0,0,0,0,0,0,0,0,0,2,2,1,1,1,2,
    0,0,0,0,0,0,0,0,0,2,1,1,1,1,2,
    0,0,0,0,0,0,0,0,2,1,1,1,2,1,2,
    0,0,0,0,0,0,0,2,1,1,1,2,2,2,2,
    2,2,2,0,0,0,2,1,1,1,2,2,0,0,0,
    2,1,1,2,0,2,1,1,1,2,0,0,0,0,0,
    0,2,1,1,2,1,1,1,2,0,0,0,0,0,0,
    0,2,1,1,1,1,1,2,0,0,0,0,0,0,0,
    2,1,2,1,1,1,2,0,0,0,0,0,0,0,0,
    2,1,1,1,1,2,0,0,0,0,0,0,0,0,0,
    2,1,1,1,2,2,0,0,0,0,0,0,0,0,0,
    2,1,1,1,1,2,0,0,0,0,0,0,0,0,0,
    2,2,2,2,2,2,0,0,0,0,0,0,0,0,0,
};

static const cursor_shape_t SHAPE_ARROW   = {12, 19, 0, 0, g_cursor_arrow};
static const cursor_shape_t SHAPE_HRESIZE = {15,  9, 7, 4, g_cursor_hresize};
static const cursor_shape_t SHAPE_VRESIZE = { 9, 15, 4, 7, g_cursor_vresize};
static const cursor_shape_t SHAPE_NWSE    = {15, 15, 7, 7, g_cursor_nwse};
static const cursor_shape_t SHAPE_NESW    = {15, 15, 7, 7, g_cursor_nesw};

// Pick the cursor shape that matches a resize-edge bitmask.
// 1=L, 2=R, 4=T, 8=B. Corners take diagonal cursors.
static const cursor_shape_t* cursor_for_edge(int edge) {
    int lr = edge & 3;
    int tb = edge & 12;
    if (lr && tb) {
        // (L|T) or (R|B) => NW-SE; (R|T) or (L|B) => NE-SW.
        if ((edge & 1) == ((edge & 4) >> 2))
            return &SHAPE_NWSE;
        return &SHAPE_NESW;
    }
    if (lr) return &SHAPE_HRESIZE;
    if (tb) return &SHAPE_VRESIZE;
    return &SHAPE_ARROW;
}

// Erase cursor by restoring saved pixels. Uses the shape that was
// actually drawn (g_cursor_saved_shape), not the current one.
static void erase_cursor(void) {
    if (!g_cursor_drawn || !g_cursor_saved_shape) return;
    const cursor_shape_t* sh = g_cursor_saved_shape;
    for (int row = 0; row < sh->h; row++) {
        int32_t py = g_cursor_save_y + row;
        if (py < 0 || (uint32_t)py >= g_fb_height) continue;
        for (int col = 0; col < sh->w; col++) {
            int32_t px = g_cursor_save_x + col;
            if (px < 0 || (uint32_t)px >= g_fb_width) continue;
            if (sh->data[row * sh->w + col] != 0)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px] =
                    g_cursor_save[row * CURSOR_MAX_W + col];
        }
    }
    g_cursor_drawn = 0;
}

static void draw_cursor(void) {
    const cursor_shape_t* sh = g_cursor_shape ? g_cursor_shape : &SHAPE_ARROW;
    int32_t ox = g_cursor_x - sh->hotx;
    int32_t oy = g_cursor_y - sh->hoty;
    // Save pixels under cursor before drawing
    for (int row = 0; row < sh->h; row++) {
        int32_t py = oy + row;
        if (py < 0 || (uint32_t)py >= g_fb_height) continue;
        for (int col = 0; col < sh->w; col++) {
            int32_t px = ox + col;
            if (px < 0 || (uint32_t)px >= g_fb_width) continue;
            if (sh->data[row * sh->w + col] != 0)
                g_cursor_save[row * CURSOR_MAX_W + col] =
                    g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px];
        }
    }
    g_cursor_save_x = ox;
    g_cursor_save_y = oy;
    g_cursor_saved_shape = sh;
    g_cursor_drawn = 1;
    for (int row = 0; row < sh->h; row++) {
        int32_t py = oy + row;
        if (py < 0 || (uint32_t)py >= g_fb_height) continue;
        for (int col = 0; col < sh->w; col++) {
            int32_t px = ox + col;
            if (px < 0 || (uint32_t)px >= g_fb_width) continue;
            uint8_t v = sh->data[row * sh->w + col];
            if (v == 1)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px] = 0x00FFFFFF;
            else if (v == 2)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px] = 0x00000000;
        }
    }
}

static void dirty_reset(void) {
    g_dirty_x0 = g_dirty_y0 = 0x7FFFFFFF;
    g_dirty_x1 = g_dirty_y1 = 0;
    g_dirty_full = 0;
}

static void dirty_add(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (x < g_dirty_x0) g_dirty_x0 = x;
    if (y < g_dirty_y0) g_dirty_y0 = y;
    if (x + (int32_t)w > g_dirty_x1) g_dirty_x1 = x + (int32_t)w;
    if (y + (int32_t)h > g_dirty_y1) g_dirty_y1 = y + (int32_t)h;
}

static void dirty_add_surface(md_surface_t* s) {
    // Include decoration area (title bar + borders)
    int32_t dx = s->x - MD_DECO_BORDER_W;
    int32_t dy = s->y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W;
    uint32_t dw = s->width + 2 * MD_DECO_BORDER_W;
    uint32_t dh = s->height + MD_DECO_TITLEBAR_H + 2 * MD_DECO_BORDER_W;
    dirty_add(dx, dy, dw, dh);
}

// ── Damage debug overlay ─────────────────────────────────────────────────
// When g_debug_damage is on, tint the region the compositor is about to
// flip so the dirty area is visible. Full-screen redraw paints a bright
// outline around the whole framebuffer; partial redraw tints the dirty
// rect itself. Toggled with F12 (scancode 0x58).

static void fb_tint_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         uint32_t tint) {
    if (x < 0) { if ((uint32_t)(-x) >= w) return; w += x; x = 0; }
    if (y < 0) { if ((uint32_t)(-y) >= h) return; h += y; y = 0; }
    if ((uint32_t)x >= g_fb_width || (uint32_t)y >= g_fb_height) return;
    if ((uint32_t)x + w > g_fb_width)  w = g_fb_width  - (uint32_t)x;
    if ((uint32_t)y + h > g_fb_height) h = g_fb_height - (uint32_t)y;
    // Simple 50%-blend tint: (px & 0xFEFEFE) >> 1  +  (tint & 0xFEFEFE) >> 1
    for (uint32_t row = 0; row < h; row++) {
        uint32_t* p = g_bb + ((uint32_t)y + row) * g_fb_stride + (uint32_t)x;
        for (uint32_t col = 0; col < w; col++) {
            uint32_t px = p[col];
            p[col] = ((px & 0xFEFEFE) >> 1) + ((tint & 0xFEFEFE) >> 1);
        }
    }
}

static void fb_stroke_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           uint32_t color, uint32_t thick) {
    if (w == 0 || h == 0) return;
    fb_fill_rect(x, y, w, thick, color);
    if (h > thick) fb_fill_rect(x, y + (int32_t)h - (int32_t)thick, w, thick, color);
    fb_fill_rect(x, y, thick, h, color);
    if (w > thick) fb_fill_rect(x + (int32_t)w - (int32_t)thick, y, thick, h, color);
}

static void draw_debug_damage_full(void) {
    if (!g_debug_damage) return;
    // Highlight the whole framebuffer with a magenta outline.
    fb_stroke_rect(0, 0, g_fb_width, g_fb_height, 0x00FF00FF, 3);
}

static void draw_debug_damage_partial(int32_t x, int32_t y,
                                      uint32_t w, uint32_t h) {
    if (!g_debug_damage) return;
    // Tint the dirty region + outline it in bright green.
    fb_tint_rect(x, y, w, h, 0x0000FF00);
    fb_stroke_rect(x, y, w, h, 0x0000FF00, 1);
}

static void composite(void) {
    // Restore backbuffer pixels under the cursor before anything else.
    // If we skip this (and just clear the flag), a partial recompose that
    // doesn't touch the cursor region will later cause draw_cursor() to
    // "save" the existing cursor pixels as the underlying content, which
    // becomes a permanent ghost cursor on the next mouse move.
    erase_cursor();

    if (g_dirty_full) {
        // Full-screen redraw (focus change, window move/create/destroy)
        fb_fill_rect(0, 0, g_fb_width, g_fb_height, MD_COLOR_BACKGROUND);

        for (md_surface_t* s = g_z_bottom; s; s = s->z_next) {
            if (!s->id || !s->visible || !s->committed || !s->attached)
                continue;
            draw_decoration(s);
            fb_blit_buffer(s->attached, s->x, s->y, 0, 0,
                           s->width, s->height);
        }

        if (g_resize_surface) {
            int32_t ox = g_resize_pending_x - MD_DECO_BORDER_W;
            int32_t oy = g_resize_pending_y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W;
            int32_t ow = g_resize_pending_w + 2 * MD_DECO_BORDER_W;
            int32_t oh = g_resize_pending_h + MD_DECO_TITLEBAR_H + 2 * MD_DECO_BORDER_W;
            fb_draw_outline(ox, oy, ow, oh, 2, 0x00FFFF00);
        }
        draw_debug_damage_full();
        draw_cursor();
        fb_flip();
    } else {
        // Partial redraw — only the dirty bounding box.
        // Clamp to screen.
        if (g_dirty_x0 < 0) g_dirty_x0 = 0;
        if (g_dirty_y0 < 0) g_dirty_y0 = 0;
        if (g_dirty_x1 > (int32_t)g_fb_width)  g_dirty_x1 = (int32_t)g_fb_width;
        if (g_dirty_y1 > (int32_t)g_fb_height) g_dirty_y1 = (int32_t)g_fb_height;

        uint32_t dw = (uint32_t)(g_dirty_x1 - g_dirty_x0);
        uint32_t dh = (uint32_t)(g_dirty_y1 - g_dirty_y0);
        if (dw == 0 || dh == 0) { g_needs_recomposite = 0; dirty_reset(); return; }

        // Clear the dirty region to background
        fb_fill_rect(g_dirty_x0, g_dirty_y0, dw, dh, MD_COLOR_BACKGROUND);

        // Redraw only surfaces that overlap the dirty region.
        // Clip each blit to the dirty bounding box to avoid redundant
        // backbuffer writes outside the region that will be flipped.
        for (md_surface_t* s = g_z_bottom; s; s = s->z_next) {
            if (!s->id || !s->visible || !s->committed || !s->attached)
                continue;
            // Surface bounding box including decorations.
            int32_t sx0 = s->x - MD_DECO_BORDER_W;
            int32_t sy0 = s->y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W;
            int32_t sx1 = s->x + (int32_t)s->width + MD_DECO_BORDER_W;
            int32_t sy1 = s->y + (int32_t)s->height + MD_DECO_BORDER_W;
            // Skip if no overlap with dirty rect.
            if (sx1 <= g_dirty_x0 || sx0 >= g_dirty_x1 ||
                sy1 <= g_dirty_y0 || sy0 >= g_dirty_y1)
                continue;
            draw_decoration(s);
            // Clip blit to dirty region for fewer backbuffer writes.
            int32_t bx0 = g_dirty_x0 > s->x ? g_dirty_x0 : s->x;
            int32_t by0 = g_dirty_y0 > s->y ? g_dirty_y0 : s->y;
            int32_t bx1 = g_dirty_x1 < s->x + (int32_t)s->width
                        ? g_dirty_x1 : s->x + (int32_t)s->width;
            int32_t by1 = g_dirty_y1 < s->y + (int32_t)s->height
                        ? g_dirty_y1 : s->y + (int32_t)s->height;
            if (bx1 > bx0 && by1 > by0) {
                fb_blit_buffer(s->attached, bx0, by0,
                               (uint32_t)(bx0 - s->x), (uint32_t)(by0 - s->y),
                               (uint32_t)(bx1 - bx0), (uint32_t)(by1 - by0));
            }
        }

        if (g_resize_surface) {
            int32_t ox = g_resize_pending_x - MD_DECO_BORDER_W;
            int32_t oy = g_resize_pending_y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W;
            int32_t ow = g_resize_pending_w + 2 * MD_DECO_BORDER_W;
            int32_t oh = g_resize_pending_h + MD_DECO_TITLEBAR_H + 2 * MD_DECO_BORDER_W;
            fb_draw_outline(ox, oy, ow, oh, 2, 0x00FFFF00);
        }
        draw_debug_damage_partial(g_dirty_x0, g_dirty_y0, dw, dh);
        draw_cursor();
        // Only flip the dirty rectangle to MMIO — the big perf win
        fb_flip_rect(g_dirty_x0, g_dirty_y0, dw, dh);
    }

    g_needs_recomposite = 0;
    dirty_reset();
}

// ── Hit testing ──────────────────────────────────────────────────────────

static md_surface_t* hit_test(int32_t px, int32_t py,
                              int* hit_titlebar, int* hit_close,
                              int* hit_maximize, int* hit_edge) {
    *hit_titlebar = 0;
    *hit_close = 0;
    *hit_maximize = 0;
    *hit_edge = 0;

    for (md_surface_t* s = g_z_top; s; s = s->z_prev) {
        if (!s->id || !s->visible || !s->committed) continue;
        // Maximized surfaces are not resizable via edge drag.
        int resizable = !s->maximized;

        // Full decorated bounding box
        int32_t bx0 = s->x - MD_DECO_BORDER_W;
        int32_t by0 = s->y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W;
        int32_t bx1 = s->x + (int32_t)s->width + MD_DECO_BORDER_W;
        int32_t by1 = s->y + (int32_t)s->height + MD_DECO_BORDER_W;

        if (px < bx0 || px >= bx1 || py < by0 || py >= by1)
            continue;

        // Edge hit detection — check before the title bar so corner grabs
        // on the top border take priority over title-bar drag. Edges are the
        // RESIZE_EDGE_GRAB-pixel band just inside the decorated bbox.
        if (resizable) {
            int edge = 0;
            if (px < bx0 + RESIZE_EDGE_GRAB) edge |= 1;       // left
            if (px >= bx1 - RESIZE_EDGE_GRAB) edge |= 2;      // right
            if (py < by0 + RESIZE_EDGE_GRAB) edge |= 4;       // top
            if (py >= by1 - RESIZE_EDGE_GRAB) edge |= 8;      // bottom
            if (edge) {
                *hit_edge = edge;
                return s;
            }
        }

        // Check title bar area
        int32_t tb_x = s->x;
        int32_t tb_y = s->y - MD_DECO_TITLEBAR_H;
        if (px >= tb_x && px < tb_x + (int32_t)s->width &&
            py >= tb_y && py < s->y) {
            // Check close button
            int32_t close_x = s->x + (int32_t)s->width - MD_DECO_BUTTON_W - 2;
            int32_t close_y = s->y - MD_DECO_TITLEBAR_H + 2;
            if (px >= close_x && px < close_x + (int32_t)MD_DECO_BUTTON_W &&
                py >= close_y && py < close_y + (int32_t)MD_DECO_BUTTON_H) {
                *hit_close = 1;
                return s;
            }
            // Check maximize button
            int32_t max_x = close_x - MD_DECO_BUTTON_W - 2;
            int32_t max_y = close_y;
            if (px >= max_x && px < max_x + (int32_t)MD_DECO_BUTTON_W &&
                py >= max_y && py < max_y + (int32_t)MD_DECO_BUTTON_H) {
                *hit_maximize = 1;
                return s;
            }
            *hit_titlebar = 1;
            return s;
        }

        // Check client area
        if (px >= s->x && px < s->x + (int32_t)s->width &&
            py >= s->y && py < s->y + (int32_t)s->height) {
            return s;
        }
    }
    return 0;
}

// ── Keyboard input handling ──────────────────────────────────────────────

static void handle_keyboard(int kb_fd) {
    // Read input_event_t structs from /dev/input/event0
    input_event_t events[16];
    int n = (int)read(kb_fd, events, sizeof(events));
    if (n <= 0) return;

    int count = n / (int)sizeof(input_event_t);
    for (int i = 0; i < count; i++) {
        input_event_t* ev = &events[i];
        if (ev->type != EV_KEY) continue;

        int pressed = (ev->value == 1);  // 1=press, 0=release, 2=repeat

        // Update modifier state
        switch (ev->code) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            if (pressed) g_mod_state |= MD_MOD_SHIFT;
            else         g_mod_state &= ~MD_MOD_SHIFT;
            break;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            if (pressed) g_mod_state |= MD_MOD_CTRL;
            else         g_mod_state &= ~MD_MOD_CTRL;
            break;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            if (pressed) g_mod_state |= MD_MOD_ALT;
            else         g_mod_state &= ~MD_MOD_ALT;
            break;
        }

        // ── F12: toggle damage debug overlay ──────────────────────────
        if (pressed && ev->code == KEY_F12) {
            g_debug_damage = !g_debug_damage;
            g_dirty_full = 1;
            g_needs_recomposite = 1;
            continue;
        }

        // ── VT switching: Ctrl+Alt+F1..F6 ──────────────────────────────
        if (pressed && (g_mod_state & MD_MOD_CTRL) && (g_mod_state & MD_MOD_ALT)) {
            if (ev->code >= KEY_F1 && ev->code <= KEY_F6) {
                if (ev->code == KEY_F1) {
                    // Switch back to display server
                    if (!g_vt_active) {
                        g_vt_active = 1;
                        g_dirty_full = 1;
                        g_needs_recomposite = 1;
                    }
                } else {
                    // Switch away from display server
                    if (g_vt_active) {
                        g_vt_active = 0;
                        // Blank the screen (black)
                        fb_fill_rect(0, 0, g_fb_width, g_fb_height, 0x00000000);
                        fb_flip();
                    }
                }
                continue;  // consume the key, don't route to clients
            }
        }

        // If display server is switched away, don't route input
        if (!g_vt_active) continue;

        // Route to focused client
        if (!g_focused || !g_focused->owner || !g_focused->owner->active)
            continue;
        if (!g_focused->owner->seat_id) continue;

        md_seat_key_t evt;
        evt.hdr = md_msg(g_focused->owner->seat_id,
                         (ev->value == 0) ? MD_SEAT_KEY_RELEASE : MD_SEAT_KEY_PRESS,
                         sizeof(evt));
        evt.keycode = ev->code;
        evt.modifiers = g_mod_state;
        send_msg(g_focused->owner, &evt, sizeof(evt));
    }
}

// ── Mouse input handling ─────────────────────────────────────────────────

static void handle_mouse(int mouse_fd) {
    mouse_event_t events[32];
    int n = (int)read(mouse_fd, events, sizeof(events));
    if (n <= 0) return;
    if (!g_vt_active) return;  // suppress mouse when VT switched away

    int count = n / (int)sizeof(mouse_event_t);
    for (int i = 0; i < count; i++) {
        mouse_event_t* ev = &events[i];

        // Update cursor position (negate: QEMU PS/2 mouse axes are inverted)
        g_cursor_x -= ev->dx;
        g_cursor_y -= ev->dy;
        // Clamp to screen
        if (g_cursor_x < 0) g_cursor_x = 0;
        if (g_cursor_y < 0) g_cursor_y = 0;
        if (g_cursor_x >= (int32_t)g_fb_width)  g_cursor_x = (int32_t)g_fb_width - 1;
        if (g_cursor_y >= (int32_t)g_fb_height) g_cursor_y = (int32_t)g_fb_height - 1;

        g_mouse_buttons_prev = g_mouse_buttons;
        g_mouse_buttons = ev->buttons;

        int left_pressed  = (g_mouse_buttons & MOUSE_BTN_LEFT) &&
                           !(g_mouse_buttons_prev & MOUSE_BTN_LEFT);
        int left_released = !(g_mouse_buttons & MOUSE_BTN_LEFT) &&
                            (g_mouse_buttons_prev & MOUSE_BTN_LEFT);

        // Handle edge resize drag — pure visual feedback until release
        if (g_resize_surface) {
            if (left_released) {
                // Apply final geometry. Use pending values from last move.
                md_surface_t* s = g_resize_surface;
                int32_t new_x = g_resize_pending_x;
                int32_t new_y = g_resize_pending_y;
                int32_t new_w = g_resize_pending_w;
                int32_t new_h = g_resize_pending_h;

                // Dirty old visible geometry + any stale outline.
                dirty_add_surface(s);
                if (g_resize_has_last) {
                    int32_t ox0 = g_resize_last_x - MD_DECO_BORDER_W - 1;
                    int32_t oy0 = g_resize_last_y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W - 1;
                    int32_t ow  = g_resize_last_w + 2 * MD_DECO_BORDER_W + 2;
                    int32_t oh  = g_resize_last_h + MD_DECO_TITLEBAR_H + 2 * MD_DECO_BORDER_W + 2;
                    dirty_add(ox0, oy0, (uint32_t)ow, (uint32_t)oh);
                }

                s->x = new_x;
                s->y = new_y;
                s->width = (uint32_t)new_w;
                s->height = (uint32_t)new_h;

                // Send configure so the client can allocate a new buffer.
                uint32_t states = s->focused ? MD_SURFACE_STATE_FOCUSED : 0;
                surface_send_configure(s, states);

                // Union of old (already dirtied above) and new decorated
                // rects is enough — we do NOT full-redraw. Partial recompose
                // handles the rest: the dirty region is background-filled,
                // then surfaces overlapping it are re-blitted.
                dirty_add_surface(s);
                g_needs_recomposite = 1;

                g_resize_surface = 0;
                g_resize_edge = 0;
                g_resize_has_last = 0;
            } else {
                // Compute proposed geometry from cursor delta + anchored edge.
                int32_t dx = g_cursor_x - g_resize_anchor_x;
                int32_t dy = g_cursor_y - g_resize_anchor_y;
                int32_t nx = g_resize_start_x;
                int32_t ny = g_resize_start_y;
                int32_t nw = (int32_t)g_resize_start_w;
                int32_t nh = (int32_t)g_resize_start_h;
                if (g_resize_edge & 1) { nx += dx; nw -= dx; }   // left
                if (g_resize_edge & 2) { nw += dx; }             // right
                if (g_resize_edge & 4) { ny += dy; nh -= dy; }   // top
                if (g_resize_edge & 8) { nh += dy; }             // bottom
                if (nw < MIN_SURFACE_W) {
                    if (g_resize_edge & 1) nx -= (MIN_SURFACE_W - nw);
                    nw = MIN_SURFACE_W;
                }
                if (nh < MIN_SURFACE_H) {
                    if (g_resize_edge & 4) ny -= (MIN_SURFACE_H - nh);
                    nh = MIN_SURFACE_H;
                }
                g_resize_pending_x = nx;
                g_resize_pending_y = ny;
                g_resize_pending_w = nw;
                g_resize_pending_h = nh;

                // Dirty the union of previous and new outline bboxes so the
                // partial recompose erases the old rubber band before we
                // redraw the new one.
                if (g_resize_has_last) {
                    int32_t ox0 = g_resize_last_x - MD_DECO_BORDER_W - 1;
                    int32_t oy0 = g_resize_last_y - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W - 1;
                    int32_t ow  = g_resize_last_w + 2 * MD_DECO_BORDER_W + 2;
                    int32_t oh  = g_resize_last_h + MD_DECO_TITLEBAR_H + 2 * MD_DECO_BORDER_W + 2;
                    dirty_add(ox0, oy0, (uint32_t)ow, (uint32_t)oh);
                }
                int32_t nx0 = nx - MD_DECO_BORDER_W - 1;
                int32_t ny0 = ny - MD_DECO_TITLEBAR_H - MD_DECO_BORDER_W - 1;
                int32_t nww = nw + 2 * MD_DECO_BORDER_W + 2;
                int32_t nhh = nh + MD_DECO_TITLEBAR_H + 2 * MD_DECO_BORDER_W + 2;
                dirty_add(nx0, ny0, (uint32_t)nww, (uint32_t)nhh);

                g_resize_last_x = nx;
                g_resize_last_y = ny;
                g_resize_last_w = nw;
                g_resize_last_h = nh;
                g_resize_has_last = 1;

                g_needs_recomposite = 1;
            }
            continue;
        }

        // Handle title bar drag
        if (g_drag_surface) {
            if (left_released) {
                g_drag_surface->moving = 0;
                g_drag_surface = 0;
            } else {
                g_drag_surface->x = g_cursor_x - g_drag_surface->drag_ox;
                g_drag_surface->y = g_cursor_y - g_drag_surface->drag_oy;
                g_dirty_full = 1;
                g_needs_recomposite = 1;
            }
            continue;
        }

        // Left button press: hit test
        if (left_pressed) {
            int hit_titlebar = 0, hit_close = 0, hit_maximize = 0, hit_edge = 0;
            md_surface_t* s = hit_test(g_cursor_x, g_cursor_y,
                                       &hit_titlebar, &hit_close,
                                       &hit_maximize, &hit_edge);
            if (s) {
                focus_surface(s);

                if (hit_edge) {
                    // Start rubber-band resize drag. We capture a snapshot of
                    // the surface geometry and the cursor anchor; nothing is
                    // mutated until release.
                    g_resize_surface = s;
                    g_resize_edge = hit_edge;
                    g_resize_start_x = s->x;
                    g_resize_start_y = s->y;
                    g_resize_start_w = s->width;
                    g_resize_start_h = s->height;
                    g_resize_anchor_x = g_cursor_x;
                    g_resize_anchor_y = g_cursor_y;
                    g_resize_pending_x = s->x;
                    g_resize_pending_y = s->y;
                    g_resize_pending_w = (int32_t)s->width;
                    g_resize_pending_h = (int32_t)s->height;
                    g_resize_has_last = 0;
                } else if (hit_close) {
                    if (s->owner && s->owner->active) {
                        if (s->owner->unresponsive && s->owner->peer_pid) {
                            // Client's event loop has been dead for
                            // MD_PING_TIMEOUT_MS and the user explicitly
                            // clicked X — this is the Windows "End Task"
                            // moment. Use the kernel-trusted peer pid
                            // (not anything client-reported) and SIGKILL.
                            // The socket tears down via normal POLLHUP,
                            // and client_read will mark the client dead.
                            kill((int)s->owner->peer_pid, SIGKILL);
                        } else {
                            md_msg_header_t close_msg = md_msg(s->id, MD_SURFACE_CLOSE,
                                                               MD_HEADER_SIZE);
                            send_msg(s->owner, &close_msg, MD_HEADER_SIZE);
                        }
                    }
                } else if (hit_maximize) {
                    surface_toggle_maximize(s);
                } else if (hit_titlebar) {
                    // Double-click on title bar could maximize too,
                    // but for now just start dragging.
                    g_drag_surface = s;
                    s->moving = 1;
                    s->drag_ox = g_cursor_x - s->x;
                    s->drag_oy = g_cursor_y - s->y;
                } else {
                    // Click in client area
                    if (s->owner && s->owner->active && s->owner->seat_id) {
                        md_seat_pointer_button_t btn;
                        btn.hdr = md_msg(s->owner->seat_id, MD_SEAT_POINTER_BUTTON,
                                         sizeof(btn));
                        btn.button = 0;
                        btn.state = 1;
                        send_msg(s->owner, &btn, sizeof(btn));
                    }
                }
            } else {
                focus_surface(0);
            }
        }

        // Left button release (not dragging)
        if (left_released) {
            if (g_focused && g_focused->owner && g_focused->owner->active &&
                g_focused->owner->seat_id) {
                md_seat_pointer_button_t btn;
                btn.hdr = md_msg(g_focused->owner->seat_id, MD_SEAT_POINTER_BUTTON,
                                 sizeof(btn));
                btn.button = 0;  // left
                btn.state = 0;   // released
                send_msg(g_focused->owner, &btn, sizeof(btn));
            }
        }

        // Right button
        int right_pressed  = (g_mouse_buttons & MOUSE_BTN_RIGHT) &&
                            !(g_mouse_buttons_prev & MOUSE_BTN_RIGHT);
        int right_released = !(g_mouse_buttons & MOUSE_BTN_RIGHT) &&
                             (g_mouse_buttons_prev & MOUSE_BTN_RIGHT);
        if (right_pressed || right_released) {
            if (g_focused && g_focused->owner && g_focused->owner->active &&
                g_focused->owner->seat_id) {
                md_seat_pointer_button_t btn;
                btn.hdr = md_msg(g_focused->owner->seat_id, MD_SEAT_POINTER_BUTTON,
                                 sizeof(btn));
                btn.button = 1;  // right
                btn.state = right_pressed ? 1 : 0;
                send_msg(g_focused->owner, &btn, sizeof(btn));
            }
        }

        // Pointer move event to focused client
        if ((ev->dx || ev->dy) && g_focused && g_focused->owner &&
            g_focused->owner->active && g_focused->owner->seat_id) {
            md_seat_pointer_move_t move;
            move.hdr = md_msg(g_focused->owner->seat_id, MD_SEAT_POINTER_MOVE,
                              sizeof(move));
            // Surface-relative coordinates
            move.x = g_cursor_x - g_focused->x;
            move.y = g_cursor_y - g_focused->y;
            send_msg(g_focused->owner, &move, sizeof(move));
        }

        // Pick cursor shape based on what's under the pointer. While a
        // resize drag is active, pin the cursor to the grabbed edge's
        // shape so it doesn't flicker to the arrow when the pointer
        // briefly leaves the edge band.
        if (g_resize_surface) {
            g_cursor_shape = cursor_for_edge(g_resize_edge);
        } else {
            int ht=0, hc=0, hm=0, he=0;
            (void)hit_test(g_cursor_x, g_cursor_y, &ht, &hc, &hm, &he);
            g_cursor_shape = he ? cursor_for_edge(he) : &SHAPE_ARROW;
        }

        // Only redraw cursor (erase old + draw new) — no full recomposite.
        // Flip the union of the old and new cursor regions to MMIO fb.
        int32_t old_x = g_cursor_save_x, old_y = g_cursor_save_y;
        uint32_t old_w = g_cursor_saved_shape ? g_cursor_saved_shape->w : 0;
        uint32_t old_h = g_cursor_saved_shape ? g_cursor_saved_shape->h : 0;
        erase_cursor();
        if (old_w && old_h) fb_flip_rect(old_x, old_y, old_w, old_h);
        draw_cursor();
        fb_flip_rect(g_cursor_save_x, g_cursor_save_y,
                     g_cursor_saved_shape->w, g_cursor_saved_shape->h);
    }
}

// ── Main ─────────────────────────────────────────────────────────────────

static void print(const char* s) {
    uint32_t len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

static void print_num(uint32_t n) {
    char buf[16];
    int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    else { while (n) { buf[i++] = '0' + (n % 10); n /= 10; } }
    for (int j = i - 1; j >= 0; j--) write(1, &buf[j], 1);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    print("makadisplay: starting compositor\n");

    // ── Map framebuffer ──────────────────────────────────────────────────
    typedef struct {
        uint64_t phys;
        uint32_t width;
        uint32_t height;
        uint32_t pitch;
        uint32_t bpp;
    } fb_info_t;

    fb_info_t info;
    {
        long r = (long)syscall1(34 /* SYS_FB_INFO */, (uint64_t)&info);
        if (r < 0) {
            print("makadisplay: fb_info failed\n");
            return 1;
        }
    }

    g_fb_width  = info.width;
    g_fb_height = info.height;
    g_fb_pitch  = info.pitch;
    g_fb_stride = info.pitch / 4;

    print("makadisplay: fb ");
    print_num(g_fb_width);
    print("x");
    print_num(g_fb_height);
    print(" pitch=");
    print_num(g_fb_pitch);
    print("\n");

    void* fb_ptr = fb_map();
    if (fb_ptr == (void*)-1L) {
        print("makadisplay: fb_map failed (must be root)\n");
        return 1;
    }
    g_fb = (uint32_t*)fb_ptr;

    // Allocate backbuffer in regular cached RAM
    uint64_t fb_size = (uint64_t)g_fb_stride * g_fb_height;
    g_bb = (uint32_t*)mmap(0, fb_size * 4, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_bb == (uint32_t*)(void*)-1L) {
        print("makadisplay: backbuffer mmap failed\n");
        return 1;
    }

    // Initialize cursor to center of screen
    g_cursor_x = (int32_t)(g_fb_width / 2);
    g_cursor_y = (int32_t)(g_fb_height / 2);
    g_cursor_shape = &SHAPE_ARROW;

    fb_fill_rect(0, 0, g_fb_width, g_fb_height, MD_COLOR_BACKGROUND);
    draw_cursor();
    fb_flip();
    print("makadisplay: framebuffer mapped, desktop drawn\n");

    // ── Create listener socket ───────────────────────────────────────────
    g_listener_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listener_fd < 0) {
        print("makadisplay: socket() failed\n");
        return 1;
    }

    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    const char* path = MD_SOCKET_PATH;
    int pi = 0;
    while (path[pi] && pi < 107) { sa.sun_path[pi] = path[pi]; pi++; }
    sa.sun_path[pi] = '\0';

    if (bind(g_listener_fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        print("makadisplay: bind() failed\n");
        close(g_listener_fd);
        return 1;
    }

    if (listen(g_listener_fd, MD_MAX_CLIENTS) < 0) {
        print("makadisplay: listen() failed\n");
        close(g_listener_fd);
        return 1;
    }

    print("makadisplay: listening on ");
    print(MD_SOCKET_PATH);
    print("\n");

    // ── Open input devices ───────────────────────────────────────────────
    int kb_fd = open("/dev/input/event0", O_RDONLY);
    if (kb_fd >= 0)
        print("makadisplay: keyboard opened (/dev/input/event0)\n");

    int mouse_fd = open("/dev/mouse", O_RDONLY);
    if (mouse_fd >= 0)
        print("makadisplay: mouse opened (/dev/mouse)\n");

    // ── Auto-launch client(s) ────────────────────────────────────────────
    {
        const char* child = "/bin/makaterm";
        if (argc > 1) child = argv[1];
        int stdio[3] = { -1, -1, -1 };
        int pid = spawn(child, 0, 0, stdio);
        if (pid > 0) {
            print("makadisplay: launched ");
            print(child);
            print("\n");
        } else {
            print("makadisplay: failed to launch ");
            print(child);
            print("\n");
        }
    }

    // ── Main event loop ──────────────────────────────────────────────────
    dirty_reset();
    for (;;) {
        // Build pollfd array: listener + keyboard + mouse + clients
        pollfd_t fds[MD_MAX_CLIENTS + 4];
        int nfds = 0;

        fds[nfds].fd = g_listener_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        int listener_idx = nfds++;

        int kb_idx = -1;
        if (kb_fd >= 0) {
            fds[nfds].fd = kb_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            kb_idx = nfds++;
        }

        int mouse_idx = -1;
        if (mouse_fd >= 0) {
            fds[nfds].fd = mouse_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            mouse_idx = nfds++;
        }

        int client_poll_idx[MD_MAX_CLIENTS];
        for (uint32_t i = 0; i < MD_MAX_CLIENTS; i++) {
            if (g_clients[i].active && !g_clients[i].dead) {
                client_poll_idx[i] = nfds;
                fds[nfds].fd = g_clients[i].fd;
                fds[nfds].events = POLLIN;
                // Also watch for POLLOUT if we have queued outgoing data.
                if (g_clients[i].tx_len > 0)
                    fds[nfds].events |= POLLOUT;
                fds[nfds].revents = 0;
                nfds++;
            } else {
                client_poll_idx[i] = -1;
            }
        }

        // Poll with a short timeout so we can recomposite even without events
        int ready = poll(fds, nfds, g_needs_recomposite ? 0 : 100);

        if (ready > 0) {
            // Accept new connections
            if (fds[listener_idx].revents & POLLIN) {
                int client_fd = accept(g_listener_fd, 0, 0);
                if (client_fd >= 0) {
                    // Make the client socket nonblocking so fan-out sends
                    // never stall the compositor on a slow peer.
                    fcntl(client_fd, F_SETFL, O_NONBLOCK);
                    md_client_t* c = client_alloc(client_fd);
                    if (c) {
                        // Trusted pid from the kernel, for liveness-driven
                        // SIGKILL on user-initiated force-close.
                        int pp = getpeerpid(client_fd);
                        c->peer_pid = (pp > 0) ? (uint32_t)pp : 0;
                        md_display_global_info_t info_msg;
                        info_msg.hdr = md_msg(0, MD_DISPLAY_GLOBAL_INFO,
                                              sizeof(info_msg));
                        info_msg.width  = g_fb_width;
                        info_msg.height = g_fb_height;
                        info_msg.format = MD_FORMAT_BGRX8888;
                        info_msg.pitch  = g_fb_pitch;
                        send_msg(c, &info_msg, sizeof(info_msg));
                    } else {
                        close(client_fd);
                    }
                }
            }

            // Read keyboard input
            if (kb_idx >= 0 && (fds[kb_idx].revents & POLLIN))
                handle_keyboard(kb_fd);

            // Read mouse input
            if (mouse_idx >= 0 && (fds[mouse_idx].revents & POLLIN))
                handle_mouse(mouse_fd);

            // Drain per-client outgoing queues on POLLOUT, then read input.
            for (uint32_t i = 0; i < MD_MAX_CLIENTS; i++) {
                if (client_poll_idx[i] < 0) continue;
                uint16_t rev = fds[client_poll_idx[i]].revents;
                if (rev & POLLOUT)
                    client_tx_flush(&g_clients[i]);
                if (rev & (POLLIN | POLLHUP | POLLERR))
                    client_read(&g_clients[i]);
            }
        }

        // Reap any clients marked dead during dispatch (EPIPE, tx overflow,
        // hangup, etc.). Done here rather than inline so we don't mutate
        // g_clients while the main loop is iterating over it.
        for (uint32_t i = 0; i < MD_MAX_CLIENTS; i++) {
            if (g_clients[i].active && g_clients[i].dead)
                client_disconnect(&g_clients[i]);
        }

        // ── Liveness tick ───────────────────────────────────────────────
        // Every MD_PING_INTERVAL_MS send a ping to each responsive client
        // that does not already have one outstanding. Any outstanding ping
        // older than MD_PING_TIMEOUT_MS flips the client to "not responding"
        // and invalidates its decorations for repaint.
        {
            uint64_t now_ms = clock_ns() / 1000000ull;
            for (uint32_t i = 0; i < MD_MAX_CLIENTS; i++) {
                md_client_t* c = &g_clients[i];
                if (!c->active || c->dead) continue;

                if (c->outstanding_serial) {
                    uint64_t age = now_ms - c->outstanding_since_ms;
                    if (!c->unresponsive && age >= MD_PING_TIMEOUT_MS) {
                        c->unresponsive = 1;
                        g_dirty_full = 1;
                        g_needs_recomposite = 1;
                    }
                } else if (now_ms - c->last_ping_sent_ms >= MD_PING_INTERVAL_MS) {
                    c->ping_serial++;
                    if (c->ping_serial == 0) c->ping_serial = 1;
                    md_display_ping_t ping;
                    ping.hdr    = md_msg(0, MD_DISPLAY_PING, sizeof(ping));
                    ping.serial = c->ping_serial;
                    ping._pad   = 0;
                    c->outstanding_serial    = c->ping_serial;
                    c->outstanding_since_ms  = now_ms;
                    c->last_ping_sent_ms     = now_ms;
                    send_msg(c, &ping, sizeof(ping));
                }
            }
        }

        // Recomposite if needed (skip when VT switched away)
        if (g_needs_recomposite && g_vt_active) {
            composite();
        }
    }

    return 0;
}
