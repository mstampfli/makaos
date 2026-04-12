// MakaDisplay Client Library — implementation

#include "libdisplay.h"

// ── Internal structures ──────────────────────────────────────────────────

#define MAX_SURFACES 16
#define MAX_BUFFERS  32

struct md_client_buffer {
    uint32_t server_id;  // compositor-assigned ID
    uint32_t width;
    uint32_t height;
    uint32_t stride;     // bytes per row
    int      shm_fd;     // shmem fd
    uint32_t* data;      // mmap'd pixel data
    uint32_t data_size;
    md_display_t* dpy;
    md_buffer_release_handler_t on_release;
};

struct md_client_surface {
    uint32_t server_id;  // compositor-assigned ID
    md_display_t* dpy;
    void*    userdata;

    // Event handlers
    md_key_handler_t              on_key;
    md_configure_handler_t        on_configure;
    md_close_handler_t            on_close;
    md_focus_handler_t            on_focus;
    md_pointer_move_handler_t     on_pointer_move;
    md_pointer_button_handler_t   on_pointer_button;
};

struct md_display {
    int      fd;          // socket to compositor
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_format;
    uint32_t fb_pitch;

    md_client_surface_t surfaces[MAX_SURFACES];
    uint32_t surface_count;

    md_client_buffer_t buffers[MAX_BUFFERS];
    uint32_t buffer_count;

    uint32_t seat_id;     // server-assigned seat object ID

    // Read buffer for partial message reads
    uint8_t  read_buf[MD_MAX_MSG_SIZE * 4];
    uint32_t read_len;
};

// ── Helpers ──────────────────────────────────────────────────────────────

static void mem_zero(void* p, uint32_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}


static void send_msg(int fd, const void* msg, uint32_t size) {
    const uint8_t* p = (const uint8_t*)msg;
    uint32_t sent = 0;
    while (sent < size) {
        int r = (int)send(fd, p + sent, size - sent, 0);
        if (r <= 0) break;
        sent += (uint32_t)r;
    }
}

static int read_exact(int fd, void* buf, uint32_t n) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;
    while (got < n) {
        int r = (int)recv(fd, p + got, n - got, 0);
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return 0;
}

// Wait for a specific event (blocking read until we get it).
//
// Care: this function can be called from inside a dispatch_event callback
// (e.g., terminal's on_configure → md_buffer_create). At that point,
// dpy->read_buf may still contain *unprocessed* messages that the parent
// md_display_dispatch loop has yet to reach. We must NOT read fresh bytes
// from the socket that would appear *before* those in wire order — so we
// append any fresh messages to the tail of read_buf and only harvest the
// OBJECT_ID reply without disturbing existing ordering.
static int wait_for_object_id(md_display_t* dpy, uint32_t* out_id) {
    for (;;) {
        // Make sure we have at least one full message at the tail of
        // read_buf that has NOT yet been dispatched by the outer loop.
        // We track the write cursor via dpy->read_len.
        // First, try to find an OBJECT_ID anywhere in the unprocessed tail.
        // But we don't know what's "unprocessed" without parser state, so
        // instead we read fresh bytes until we find an OBJECT_ID reply in
        // the newly arrived stream, and append everything in between to
        // read_buf so the outer dispatch loop sees it later.
        md_msg_header_t hdr;
        if (read_exact(dpy->fd, &hdr, MD_HEADER_SIZE) < 0) return -1;
        if (hdr.size < MD_HEADER_SIZE || hdr.size > MD_MAX_MSG_SIZE) return -1;

        uint32_t payload_size = hdr.size - MD_HEADER_SIZE;
        uint8_t payload[MD_MAX_MSG_SIZE];
        if (payload_size > 0) {
            if (read_exact(dpy->fd, payload, payload_size) < 0) return -1;
        }

        if (hdr.object_id == 0 && hdr.opcode == MD_DISPLAY_OBJECT_ID) {
            if (payload_size >= 8) {
                uint32_t* p32 = (uint32_t*)payload;
                *out_id = p32[0];
                return 0;
            }
            return -1;
        }

        // Not the reply — queue it into read_buf so the outer dispatcher
        // can process it after we return. Drop the event if there is no
        // room (unfortunate but better than crashing).
        uint32_t needed = hdr.size;
        if (dpy->read_len + needed > sizeof(dpy->read_buf)) continue;
        uint8_t* dst = dpy->read_buf + dpy->read_len;
        uint8_t* src = (uint8_t*)&hdr;
        for (uint32_t i = 0; i < MD_HEADER_SIZE; i++) dst[i] = src[i];
        for (uint32_t i = 0; i < payload_size; i++) dst[MD_HEADER_SIZE + i] = payload[i];
        dpy->read_len += needed;
    }
}

// ── Find surface/buffer by server ID ─────────────────────────────────────

static md_client_surface_t* find_surface(md_display_t* dpy, uint32_t id) {
    for (uint32_t i = 0; i < MAX_SURFACES; i++)
        if (dpy->surfaces[i].server_id == id) return &dpy->surfaces[i];
    return 0;
}

static md_client_buffer_t* find_buffer(md_display_t* dpy, uint32_t id) {
    for (uint32_t i = 0; i < MAX_BUFFERS; i++)
        if (dpy->buffers[i].server_id == id) return &dpy->buffers[i];
    return 0;
}

// ── Connection ───────────────────────────────────────────────────────────

md_display_t* md_display_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    const char* path = MD_SOCKET_PATH;
    int i = 0;
    while (path[i] && i < 107) { sa.sun_path[i] = path[i]; i++; }
    sa.sun_path[i] = '\0';

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return 0;
    }

    // Allocate display struct (on the heap via brk)
    md_display_t* dpy = (md_display_t*)mmap(0, sizeof(md_display_t),
                                            PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS,
                                            -1, 0);
    if (dpy == (void*)-1L) {
        close(fd);
        return 0;
    }
    mem_zero(dpy, sizeof(md_display_t));
    dpy->fd = fd;

    // Read the global_info message that the compositor sends on connect
    md_display_global_info_t info;
    if (read_exact(fd, &info, sizeof(info)) < 0) {
        close(fd);
        munmap(dpy, sizeof(md_display_t));
        return 0;
    }
    dpy->fb_width  = info.width;
    dpy->fb_height = info.height;
    dpy->fb_format = info.format;
    dpy->fb_pitch  = info.pitch;

    return dpy;
}

void md_display_disconnect(md_display_t* dpy) {
    if (!dpy) return;
    // Destroy all buffers
    for (uint32_t i = 0; i < MAX_BUFFERS; i++) {
        if (dpy->buffers[i].server_id)
            md_buffer_destroy(&dpy->buffers[i]);
    }
    close(dpy->fd);
    munmap(dpy, sizeof(md_display_t));
}

uint32_t md_display_width(md_display_t* dpy)  { return dpy->fb_width; }
uint32_t md_display_height(md_display_t* dpy) { return dpy->fb_height; }
int md_display_fd(md_display_t* dpy) { return dpy->fd; }

// ── Event dispatch ───────────────────────────────────────────────────────

static void dispatch_event(md_display_t* dpy, md_msg_header_t* hdr,
                           uint8_t* payload, uint32_t payload_len) {
    // Surface events
    md_client_surface_t* surf = find_surface(dpy, hdr->object_id);
    if (surf) {
        switch (hdr->opcode) {
        case MD_SURFACE_CONFIGURE:
            if (payload_len >= 12 && surf->on_configure) {
                uint32_t* p = (uint32_t*)payload;
                surf->on_configure(surf, p[0], p[1], p[2]);
            }
            break;
        case MD_SURFACE_CLOSE:
            if (surf->on_close) surf->on_close(surf);
            break;
        case MD_SURFACE_ENTER:
            if (surf->on_focus) surf->on_focus(surf, 1);
            break;
        case MD_SURFACE_LEAVE:
            if (surf->on_focus) surf->on_focus(surf, 0);
            break;
        }
        return;
    }

    // Seat events (key/pointer)
    if (dpy->seat_id && hdr->object_id == dpy->seat_id) {
        switch (hdr->opcode) {
        case MD_SEAT_KEY_PRESS:
        case MD_SEAT_KEY_RELEASE:
            if (payload_len >= 8) {
                uint32_t* p = (uint32_t*)payload;
                uint32_t keycode = p[0];
                uint32_t modifiers = p[1];
                int pressed = (hdr->opcode == MD_SEAT_KEY_PRESS);
                for (uint32_t i = 0; i < MAX_SURFACES; i++) {
                    if (dpy->surfaces[i].server_id && dpy->surfaces[i].on_key) {
                        dpy->surfaces[i].on_key(&dpy->surfaces[i],
                                                keycode, modifiers, pressed);
                        break;
                    }
                }
            }
            break;
        case MD_SEAT_POINTER_MOVE:
            if (payload_len >= 8) {
                int32_t* p = (int32_t*)payload;
                for (uint32_t i = 0; i < MAX_SURFACES; i++) {
                    if (dpy->surfaces[i].server_id && dpy->surfaces[i].on_pointer_move) {
                        dpy->surfaces[i].on_pointer_move(&dpy->surfaces[i], p[0], p[1]);
                        break;
                    }
                }
            }
            break;
        case MD_SEAT_POINTER_BUTTON:
            if (payload_len >= 8) {
                uint32_t* p = (uint32_t*)payload;
                uint32_t button = p[0];
                int pressed = (int)p[1];
                for (uint32_t i = 0; i < MAX_SURFACES; i++) {
                    if (dpy->surfaces[i].server_id && dpy->surfaces[i].on_pointer_button) {
                        dpy->surfaces[i].on_pointer_button(&dpy->surfaces[i], button, pressed);
                        break;
                    }
                }
            }
            break;
        }
        return;
    }

    // Buffer events (release)
    md_client_buffer_t* buf = find_buffer(dpy, hdr->object_id);
    if (buf) {
        if (hdr->opcode == MD_BUFFER_RELEASE && buf->on_release)
            buf->on_release(buf);
        return;
    }

    // Display events
    if (hdr->object_id == 0) {
        switch (hdr->opcode) {
        case MD_DISPLAY_ERROR:
            // Could log, for now ignore
            break;
        }
    }
}

int md_display_dispatch(md_display_t* dpy) {
    // Check if data is available (non-blocking poll with 0 timeout)
    pollfd_t pfd;
    pfd.fd = dpy->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, 0);
    if (pr <= 0) return 0;  // no data ready
    if (pfd.revents & (POLLHUP | POLLERR)) return -1;

    uint32_t space = sizeof(dpy->read_buf) - dpy->read_len;
    if (space == 0) { dpy->read_len = 0; space = sizeof(dpy->read_buf); }

    int n = (int)recv(dpy->fd, dpy->read_buf + dpy->read_len, space, 0);
    if (n < 0) return 0;   // EAGAIN or no data
    if (n == 0) return -1;  // server disconnected
    dpy->read_len += (uint32_t)n;

    int events = 0;
    uint32_t pos = 0;
    while (pos + MD_HEADER_SIZE <= dpy->read_len) {
        md_msg_header_t* hdr = (md_msg_header_t*)(dpy->read_buf + pos);
        if (hdr->size < MD_HEADER_SIZE || hdr->size > MD_MAX_MSG_SIZE) return -1;
        if (pos + hdr->size > dpy->read_len) break;

        uint32_t payload_len = hdr->size - MD_HEADER_SIZE;
        dispatch_event(dpy, hdr, dpy->read_buf + pos + MD_HEADER_SIZE, payload_len);
        pos += hdr->size;
        events++;
    }

    // Shift remaining data
    if (pos > 0) {
        uint32_t remaining = dpy->read_len - pos;
        for (uint32_t i = 0; i < remaining; i++)
            dpy->read_buf[i] = dpy->read_buf[pos + i];
        dpy->read_len = remaining;
    }

    return events;
}

int md_display_dispatch_blocking(md_display_t* dpy) {
    // Wait for data with poll
    pollfd_t pfd;
    pfd.fd = dpy->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r = poll(&pfd, 1, -1);
    if (r <= 0) return -1;
    return md_display_dispatch(dpy);
}

// ── Surfaces ─────────────────────────────────────────────────────────────

md_client_surface_t* md_surface_create(md_display_t* dpy) {
    // Find a free slot
    md_client_surface_t* surf = 0;
    for (uint32_t i = 0; i < MAX_SURFACES; i++) {
        if (!dpy->surfaces[i].server_id) {
            surf = &dpy->surfaces[i];
            break;
        }
    }
    if (!surf) return 0;

    // Send get_surface request
    md_msg_header_t req = md_msg(0, MD_DISPLAY_GET_SURFACE, MD_HEADER_SIZE);
    send_msg(dpy->fd, &req, MD_HEADER_SIZE);

    // Wait for object_id response
    uint32_t id = 0;
    if (wait_for_object_id(dpy, &id) < 0) return 0;

    mem_zero(surf, sizeof(md_client_surface_t));
    surf->server_id = id;
    surf->dpy = dpy;
    dpy->surface_count++;

    // Request a seat too if we don't have one
    if (!dpy->seat_id) {
        md_msg_header_t seat_req = md_msg(0, MD_DISPLAY_GET_SEAT, MD_HEADER_SIZE);
        send_msg(dpy->fd, &seat_req, MD_HEADER_SIZE);
        uint32_t seat_id = 0;
        if (wait_for_object_id(dpy, &seat_id) == 0) {
            dpy->seat_id = seat_id;
        }
    }

    return surf;
}

void md_surface_destroy(md_client_surface_t* surf) {
    if (!surf || !surf->server_id) return;
    md_msg_header_t req = md_msg(surf->server_id, MD_SURFACE_DESTROY, MD_HEADER_SIZE);
    send_msg(surf->dpy->fd, &req, MD_HEADER_SIZE);
    surf->server_id = 0;
    surf->dpy->surface_count--;
}

void md_surface_attach(md_client_surface_t* surf, md_client_buffer_t* buf) {
    if (!surf || !buf) return;
    md_surface_attach_t msg;
    msg.hdr = md_msg(surf->server_id, MD_SURFACE_ATTACH, sizeof(msg));
    msg.buffer_id = buf->server_id;
    msg._pad = 0;
    send_msg(surf->dpy->fd, &msg, sizeof(msg));
}

void md_surface_damage(md_client_surface_t* surf,
                       int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!surf) return;
    md_surface_damage_t msg;
    msg.hdr = md_msg(surf->server_id, MD_SURFACE_DAMAGE, sizeof(msg));
    msg.x = x;
    msg.y = y;
    msg.width = w;
    msg.height = h;
    send_msg(surf->dpy->fd, &msg, sizeof(msg));
}

void md_surface_commit(md_client_surface_t* surf) {
    if (!surf) return;
    md_msg_header_t msg = md_msg(surf->server_id, MD_SURFACE_COMMIT, MD_HEADER_SIZE);
    send_msg(surf->dpy->fd, &msg, MD_HEADER_SIZE);
}

void md_surface_set_title(md_client_surface_t* surf, const char* title) {
    if (!surf) return;
    md_surface_set_title_t msg;
    mem_zero(&msg, sizeof(msg));
    msg.hdr = md_msg(surf->server_id, MD_SURFACE_SET_TITLE, sizeof(msg));
    uint32_t len = 0;
    while (title[len] && len < MD_TITLE_MAX) {
        msg.title[len] = title[len];
        len++;
    }
    msg.title[len] = '\0';
    msg.len = len;
    send_msg(surf->dpy->fd, &msg, sizeof(msg));
}

uint32_t md_surface_id(md_client_surface_t* surf) {
    return surf ? surf->server_id : 0;
}

void md_surface_set_userdata(md_client_surface_t* surf, void* data) {
    if (surf) surf->userdata = data;
}

void* md_surface_get_userdata(md_client_surface_t* surf) {
    return surf ? surf->userdata : 0;
}

// ── Buffers ──────────────────────────────────────────────────────────────

md_client_buffer_t* md_buffer_create(md_display_t* dpy,
                                     uint32_t width, uint32_t height) {
    // Find free slot
    md_client_buffer_t* buf = 0;
    for (uint32_t i = 0; i < MAX_BUFFERS; i++) {
        if (!dpy->buffers[i].server_id) {
            buf = &dpy->buffers[i];
            break;
        }
    }
    if (!buf) return 0;

    mem_zero(buf, sizeof(md_client_buffer_t));
    buf->dpy    = dpy;
    buf->width  = width;
    buf->height = height;
    buf->stride = width * 4;  // BGRX8888

    // Create shared memory — each buffer needs a UNIQUE name, otherwise
    // a second shm_open of the same name re-opens the existing object and
    // subsequent ftruncate corrupts every prior mapping.
    static uint32_t s_shm_seq;
    char shm_name[32];
    shm_name[0] = '/'; shm_name[1] = 'm'; shm_name[2] = 'd';
    shm_name[3] = '_'; shm_name[4] = 'b'; shm_name[5] = 'u'; shm_name[6] = 'f';
    shm_name[7] = '_';
    uint32_t seq = ++s_shm_seq;
    int pos = 8;
    // Write seq in decimal.
    char digs[12];
    int dn = 0;
    if (seq == 0) digs[dn++] = '0';
    while (seq) { digs[dn++] = '0' + (seq % 10); seq /= 10; }
    while (dn > 0) shm_name[pos++] = digs[--dn];
    shm_name[pos] = '\0';

    uint32_t total_size = buf->stride * height;
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (shm_fd < 0) return 0;

    // Resize shmem to fit the buffer
    if (ftruncate(shm_fd, total_size) < 0) {
        close(shm_fd);
        return 0;
    }
    // Map into our address space
    void* data = mmap(0, total_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, shm_fd, 0);
    if (data == (void*)-1L) {
        close(shm_fd);
        return 0;
    }
    buf->data = (uint32_t*)data;
    buf->data_size = total_size;
    buf->shm_fd = shm_fd;

    // Send buffer create message to compositor
    md_buffer_create_t msg;
    mem_zero(&msg, sizeof(msg));
    msg.hdr = md_msg(0, MD_DISPLAY_CREATE_BUFFER, sizeof(msg));
    msg.width  = width;
    msg.height = height;
    msg.stride = buf->stride;
    msg.format = MD_FORMAT_BGRX8888;
    msg.offset = 0;
    send_msg(dpy->fd, &msg, sizeof(msg));

    // Send the shmem fd via SCM_RIGHTS
    if (sendfd(dpy->fd, shm_fd, 0xFFFFFFFF) < 0) {
        munmap(data, total_size);
        close(shm_fd);
        mem_zero(buf, sizeof(md_client_buffer_t));
        return 0;
    }
    // Wait for object_id response
    uint32_t id = 0;
    if (wait_for_object_id(dpy, &id) < 0) {
        munmap(data, total_size);
        close(shm_fd);
        mem_zero(buf, sizeof(md_client_buffer_t));
        return 0;
    }
    buf->server_id = id;
    dpy->buffer_count++;

    return buf;
}

void md_buffer_destroy(md_client_buffer_t* buf) {
    if (!buf || !buf->server_id) return;

    // Tell compositor to release
    md_msg_header_t req = md_msg(buf->server_id, MD_BUFFER_DESTROY, MD_HEADER_SIZE);
    send_msg(buf->dpy->fd, &req, MD_HEADER_SIZE);

    if (buf->data) {
        munmap(buf->data, buf->data_size);
        buf->data = 0;
    }
    if (buf->shm_fd >= 0) {
        close(buf->shm_fd);
        buf->shm_fd = -1;
    }
    buf->dpy->buffer_count--;
    buf->server_id = 0;
}

uint32_t* md_buffer_data(md_client_buffer_t* buf) {
    return buf ? buf->data : 0;
}
uint32_t md_buffer_width(md_client_buffer_t* buf) {
    return buf ? buf->width : 0;
}
uint32_t md_buffer_height(md_client_buffer_t* buf) {
    return buf ? buf->height : 0;
}
uint32_t md_buffer_stride(md_client_buffer_t* buf) {
    return buf ? buf->stride : 0;
}

// ── Event handler registration ───────────────────────────────────────────

void md_surface_on_key(md_client_surface_t* surf, md_key_handler_t handler) {
    if (surf) surf->on_key = handler;
}
void md_surface_on_configure(md_client_surface_t* surf, md_configure_handler_t handler) {
    if (surf) surf->on_configure = handler;
}
void md_surface_on_close(md_client_surface_t* surf, md_close_handler_t handler) {
    if (surf) surf->on_close = handler;
}
void md_surface_on_focus(md_client_surface_t* surf, md_focus_handler_t handler) {
    if (surf) surf->on_focus = handler;
}
void md_surface_on_pointer_move(md_client_surface_t* surf, md_pointer_move_handler_t handler) {
    if (surf) surf->on_pointer_move = handler;
}
void md_surface_on_pointer_button(md_client_surface_t* surf, md_pointer_button_handler_t handler) {
    if (surf) surf->on_pointer_button = handler;
}
void md_buffer_on_release(md_client_buffer_t* buf, md_buffer_release_handler_t handler) {
    if (buf) buf->on_release = handler;
}
