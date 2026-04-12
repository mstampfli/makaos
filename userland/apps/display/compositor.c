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

// ── Server-side surface object ───────────────────────────────────────────

struct md_surface {
    uint32_t    id;
    int32_t     x, y;           // position on screen (client content origin)
    uint32_t    width, height;  // client content area size
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
};

// ── Client connection ────────────────────────────────────────────────────

struct md_client {
    int          fd;
    int          active;
    uint32_t     next_id;    // next server-allocated (even) object id
    md_surface_t surfaces[MD_MAX_SURFACES];
    uint32_t     surface_count;
    md_buffer_t  buffers[MD_MAX_BUFFERS];
    uint32_t     buffer_count;
    uint32_t     seat_id;    // 0 if no seat allocated
    // Read buffer for partial message reads
    uint8_t      read_buf[MD_MAX_MSG_SIZE * 2];
    uint32_t     read_len;
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
static void dirty_reset(void);
static void dirty_add(int32_t x, int32_t y, uint32_t w, uint32_t h);
static void dirty_add_surface(md_surface_t* s);

// ── Mouse / cursor state ─────────────────────────────────────────────────

#define CURSOR_W 12
#define CURSOR_H 19

static int32_t  g_cursor_x;
static int32_t  g_cursor_y;
static uint8_t  g_mouse_buttons;     // current button state
static uint8_t  g_mouse_buttons_prev; // previous button state

// Cursor save/restore — avoids full recomposite on every mouse move
static uint32_t g_cursor_save[CURSOR_W * CURSOR_H]; // pixels under cursor
static int32_t  g_cursor_save_x;  // position where cursor was last drawn
static int32_t  g_cursor_save_y;
static int       g_cursor_drawn;   // whether save buffer is valid

// Dragging state
static md_surface_t* g_drag_surface;  // surface being dragged (title bar move)

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

static void send_msg(int fd, const void* msg, uint32_t size);

static void focus_surface(md_surface_t* s) {
    if (g_focused == s) return;

    // Send leave to old focused surface
    if (g_focused && g_focused->owner && g_focused->owner->active) {
        g_focused->focused = 0;
        md_msg_header_t leave = md_msg(g_focused->id, MD_SURFACE_LEAVE, MD_HEADER_SIZE);
        send_msg(g_focused->owner->fd, &leave, MD_HEADER_SIZE);
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
            send_msg(s->owner->fd, &enter, MD_HEADER_SIZE);
        }
    }
    g_dirty_full = 1;
    g_needs_recomposite = 1;
}

// ── Message sending ──────────────────────────────────────────────────────

static void send_msg(int fd, const void* msg, uint32_t size) {
    const uint8_t* p = (const uint8_t*)msg;
    uint32_t sent = 0;
    while (sent < size) {
        int r = (int)send(fd, p + sent, size - sent, 0);
        if (r <= 0) break;
        sent += (uint32_t)r;
    }
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
    c->active = 0;
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
    send_msg(s->owner->fd, &cfg, sizeof(cfg));
}

static void surface_destroy(md_surface_t* s) {
    if (!s || !s->id) return;
    int was_focused = (g_focused == s);
    if (g_focused == s) g_focused = 0;
    if (g_drag_surface == s) g_drag_surface = 0;
    z_remove(s);
    s->id = 0;
    s->attached = 0;
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
        send_msg(c->fd, &resp, sizeof(resp));
        // Send initial configure event
        surface_send_configure(s, MD_SURFACE_STATE_FOCUSED);
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
        send_msg(c->fd, &resp, sizeof(resp));
        break;
    }
    case MD_DISPLAY_SYNC: {
        md_msg_header_t done = md_msg(0, MD_DISPLAY_DONE, MD_HEADER_SIZE);
        send_msg(c->fd, &done, MD_HEADER_SIZE);
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
        send_msg(c->fd, &resp, sizeof(resp));
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
    case MD_SURFACE_DAMAGE:
        // We recomposite the whole surface for now; damage tracking is Phase 10
        break;
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
                send_msg(prev->owner->fd, &rel, MD_HEADER_SIZE);
            }
            int first_commit = !s->committed;
            s->committed = 1;
            s->width = s->attached->width;
            s->height = s->attached->height;
            // First commit: make visible and focus
            if (first_commit) {
                s->visible = 1;
                focus_surface(s);
            }
            dirty_add_surface(s);
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
    uint32_t title_color = s->focused
        ? MD_COLOR_TITLEBAR_ACTIVE
        : MD_COLOR_TITLEBAR_INACTIVE;

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
}

// ── Cursor rendering ─────────────────────────────────────────────────────
// Simple 12x19 arrow cursor.  1 = white, 2 = black outline, 0 = transparent.

static const uint8_t g_cursor_data[19][12] = {
    {2,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,0,0,0,0,0,0,0,0,0,0},
    {2,1,2,0,0,0,0,0,0,0,0,0},
    {2,1,1,2,0,0,0,0,0,0,0,0},
    {2,1,1,1,2,0,0,0,0,0,0,0},
    {2,1,1,1,1,2,0,0,0,0,0,0},
    {2,1,1,1,1,1,2,0,0,0,0,0},
    {2,1,1,1,1,1,1,2,0,0,0,0},
    {2,1,1,1,1,1,1,1,2,0,0,0},
    {2,1,1,1,1,1,1,1,1,2,0,0},
    {2,1,1,1,1,1,1,1,1,1,2,0},
    {2,1,1,1,1,1,1,2,2,2,2,0},
    {2,1,1,1,2,1,1,2,0,0,0,0},
    {2,1,1,2,0,2,1,1,2,0,0,0},
    {2,1,2,0,0,2,1,1,2,0,0,0},
    {2,2,0,0,0,0,2,1,1,2,0,0},
    {2,0,0,0,0,0,2,1,1,2,0,0},
    {0,0,0,0,0,0,0,2,1,2,0,0},
    {0,0,0,0,0,0,0,0,2,0,0,0},
};

// Erase cursor by restoring saved pixels
static void erase_cursor(void) {
    if (!g_cursor_drawn) return;
    for (int row = 0; row < CURSOR_H; row++) {
        int32_t py = g_cursor_save_y + row;
        if (py < 0 || (uint32_t)py >= g_fb_height) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int32_t px = g_cursor_save_x + col;
            if (px < 0 || (uint32_t)px >= g_fb_width) continue;
            if (g_cursor_data[row][col] != 0)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px] = g_cursor_save[row * CURSOR_W + col];
        }
    }
    g_cursor_drawn = 0;
}

static void draw_cursor(void) {
    // Save pixels under cursor before drawing
    for (int row = 0; row < CURSOR_H; row++) {
        int32_t py = g_cursor_y + row;
        if (py < 0 || (uint32_t)py >= g_fb_height) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int32_t px = g_cursor_x + col;
            if (px < 0 || (uint32_t)px >= g_fb_width) continue;
            if (g_cursor_data[row][col] != 0)
                g_cursor_save[row * CURSOR_W + col] = g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px];
        }
    }
    g_cursor_save_x = g_cursor_x;
    g_cursor_save_y = g_cursor_y;
    g_cursor_drawn = 1;
    for (int row = 0; row < CURSOR_H; row++) {
        int32_t py = g_cursor_y + row;
        if (py < 0 || (uint32_t)py >= g_fb_height) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int32_t px = g_cursor_x + col;
            if (px < 0 || (uint32_t)px >= g_fb_width) continue;
            uint8_t v = g_cursor_data[row][col];
            if (v == 1)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px] = 0x00FFFFFF; // white
            else if (v == 2)
                g_bb[(uint32_t)py * g_fb_stride + (uint32_t)px] = 0x00000000; // black
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

static void composite(void) {
    g_cursor_drawn = 0;

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

        // Redraw all surfaces that overlap the dirty region
        for (md_surface_t* s = g_z_bottom; s; s = s->z_next) {
            if (!s->id || !s->visible || !s->committed || !s->attached)
                continue;
            draw_decoration(s);
            fb_blit_buffer(s->attached, s->x, s->y, 0, 0,
                           s->width, s->height);
        }

        draw_cursor();
        // Only flip the dirty rectangle to MMIO — the big perf win
        fb_flip_rect(g_dirty_x0, g_dirty_y0, dw, dh);
    }

    g_needs_recomposite = 0;
    dirty_reset();
}

// ── Hit testing ──────────────────────────────────────────────────────────

static md_surface_t* hit_test(int32_t px, int32_t py,
                              int* hit_titlebar, int* hit_close) {
    *hit_titlebar = 0;
    *hit_close = 0;

    for (md_surface_t* s = g_z_top; s; s = s->z_prev) {
        if (!s->id || !s->visible || !s->committed) continue;

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
            } else {
                *hit_titlebar = 1;
            }
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
        send_msg(g_focused->owner->fd, &evt, sizeof(evt));
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

        // Handle title bar drag
        if (g_drag_surface) {
            if (left_released) {
                // End drag
                g_drag_surface->moving = 0;
                g_drag_surface = 0;
            } else {
                // Continue drag — move the surface
                g_drag_surface->x = g_cursor_x - g_drag_surface->drag_ox;
                g_drag_surface->y = g_cursor_y - g_drag_surface->drag_oy;
                g_dirty_full = 1;
                g_needs_recomposite = 1;
            }
            continue;
        }

        // Left button press: hit test
        if (left_pressed) {
            int hit_titlebar = 0, hit_close = 0;
            md_surface_t* s = hit_test(g_cursor_x, g_cursor_y,
                                       &hit_titlebar, &hit_close);
            if (s) {
                // Focus this surface
                focus_surface(s);

                if (hit_close) {
                    // Send close event to client
                    if (s->owner && s->owner->active) {
                        md_msg_header_t close_msg = md_msg(s->id, MD_SURFACE_CLOSE,
                                                           MD_HEADER_SIZE);
                        send_msg(s->owner->fd, &close_msg, MD_HEADER_SIZE);
                    }
                } else if (hit_titlebar) {
                    // Start dragging
                    g_drag_surface = s;
                    s->moving = 1;
                    s->drag_ox = g_cursor_x - s->x;
                    s->drag_oy = g_cursor_y - s->y;
                } else {
                    // Click in client area — send pointer button event
                    if (s->owner && s->owner->active && s->owner->seat_id) {
                        md_seat_pointer_button_t btn;
                        btn.hdr = md_msg(s->owner->seat_id, MD_SEAT_POINTER_BUTTON,
                                         sizeof(btn));
                        btn.button = 0;  // left
                        btn.state = 1;   // pressed
                        send_msg(s->owner->fd, &btn, sizeof(btn));
                    }
                }
            } else {
                // Click on desktop — unfocus
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
                send_msg(g_focused->owner->fd, &btn, sizeof(btn));
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
                send_msg(g_focused->owner->fd, &btn, sizeof(btn));
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
            send_msg(g_focused->owner->fd, &move, sizeof(move));
        }

        // Only redraw cursor (erase old + draw new) — no full recomposite
        // Flip only the old and new cursor regions to MMIO fb
        int32_t old_x = g_cursor_save_x, old_y = g_cursor_save_y;
        erase_cursor();
        fb_flip_rect(old_x, old_y, CURSOR_W, CURSOR_H);
        draw_cursor();
        fb_flip_rect(g_cursor_x, g_cursor_y, CURSOR_W, CURSOR_H);
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
            if (g_clients[i].active) {
                client_poll_idx[i] = nfds;
                fds[nfds].fd = g_clients[i].fd;
                fds[nfds].events = POLLIN;
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
                    md_client_t* c = client_alloc(client_fd);
                    if (c) {
                        md_display_global_info_t info_msg;
                        info_msg.hdr = md_msg(0, MD_DISPLAY_GLOBAL_INFO,
                                              sizeof(info_msg));
                        info_msg.width  = g_fb_width;
                        info_msg.height = g_fb_height;
                        info_msg.format = MD_FORMAT_BGRX8888;
                        info_msg.pitch  = g_fb_pitch;
                        send_msg(c->fd, &info_msg, sizeof(info_msg));
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

            // Read client data
            for (uint32_t i = 0; i < MD_MAX_CLIENTS; i++) {
                if (client_poll_idx[i] >= 0 &&
                    (fds[client_poll_idx[i]].revents & (POLLIN | POLLHUP | POLLERR))) {
                    client_read(&g_clients[i]);
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
