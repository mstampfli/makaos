#pragma once
#include "../../libc/libc.h"

// ── MakaDisplay Protocol ─────────────────────────────────────────────────
//
// Binary message protocol between display server (compositor) and clients.
// Transported over AF_UNIX SOCK_STREAM sockets.
//
// Every message starts with md_msg_header_t.  Object IDs identify protocol
// objects: the global display (id=0), surfaces, buffers, and seats.
//
// Server allocates even IDs (2,4,6,...), client allocates odd IDs (1,3,5,...).
// Object 0 is always md_display (implicit, never created).

// ── Message header ───────────────────────────────────────────────────────

typedef struct __attribute__((packed)) {
    uint32_t object_id;   // target protocol object (0 = display)
    uint16_t opcode;      // method/event on that object type
    uint16_t size;        // total message size including this header
} md_msg_header_t;

#define MD_HEADER_SIZE sizeof(md_msg_header_t)  // 8 bytes

// ── Object types ─────────────────────────────────────────────────────────
// Stored server-side to dispatch by object_id → type.

#define MD_OBJ_DISPLAY  0
#define MD_OBJ_SURFACE  1
#define MD_OBJ_BUFFER   2
#define MD_OBJ_SEAT     3

// ── Pixel formats ────────────────────────────────────────────────────────

#define MD_FORMAT_BGRX8888  0   // native MakaOS framebuffer: B,G,R,X in memory

// ── md_display opcodes (object_id = 0) ───────────────────────────────────

// Client → Server requests
#define MD_DISPLAY_GET_SURFACE  0   // → server creates surface, replies with id
#define MD_DISPLAY_GET_SEAT     1   // → server creates seat proxy, replies with id
#define MD_DISPLAY_SYNC         2   // → server replies with MD_DISPLAY_DONE
#define MD_DISPLAY_CREATE_BUFFER 3  // → create buffer (+ sendfd for shmem), replies with id

// Server → Client events
#define MD_DISPLAY_ERROR        0   // error notification
#define MD_DISPLAY_GLOBAL_INFO  1   // fb_width, fb_height, fb_format
#define MD_DISPLAY_OBJECT_ID    2   // newly allocated object id (response to get_*)
#define MD_DISPLAY_DONE         3   // sync complete

// get_surface: no payload beyond header
// get_seat:    no payload beyond header

// global_info payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t width;
    uint32_t height;
    uint32_t format;    // MD_FORMAT_*
    uint32_t pitch;     // bytes per scanline
} md_display_global_info_t;

// object_id payload (response to get_surface / get_seat)
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t new_id;
    uint32_t obj_type;  // MD_OBJ_*
} md_display_object_id_t;

// error payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t object_id;
    uint32_t code;
    char     message[128];
} md_display_error_t;

// Error codes
#define MD_ERR_INVALID_OBJECT   1
#define MD_ERR_INVALID_METHOD   2
#define MD_ERR_NO_MEMORY        3
#define MD_ERR_PERMISSION       4
#define MD_ERR_INVALID_FORMAT   5

// ── md_surface opcodes ───────────────────────────────────────────────────

// Client → Server requests
#define MD_SURFACE_ATTACH       0   // attach buffer to surface
#define MD_SURFACE_DAMAGE       1   // mark damaged rectangle
#define MD_SURFACE_COMMIT       2   // present the attached buffer
#define MD_SURFACE_SET_TITLE    3   // set window title
#define MD_SURFACE_SET_POSITION 4   // hint: preferred position
#define MD_SURFACE_DESTROY      5   // destroy this surface

// Server → Client events
#define MD_SURFACE_CONFIGURE    0   // compositor suggests size + state
#define MD_SURFACE_CLOSE        1   // compositor requests graceful close
#define MD_SURFACE_ENTER        2   // surface gained focus
#define MD_SURFACE_LEAVE        3   // surface lost focus

// Surface state bits (configure event states_mask)
#define MD_SURFACE_STATE_FOCUSED    (1u << 0)
#define MD_SURFACE_STATE_MAXIMIZED  (1u << 1)
#define MD_SURFACE_STATE_FULLSCREEN (1u << 2)

// attach payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t buffer_id;
    uint32_t _pad;
} md_surface_attach_t;

// damage payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
} md_surface_damage_t;

// commit: no payload beyond header

// set_title payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t len;       // title string length (max 63)
    char     title[64]; // null-terminated
} md_surface_set_title_t;

// set_position payload (hint only — compositor may override)
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    int32_t x;
    int32_t y;
} md_surface_set_position_t;

// configure event payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t width;
    uint32_t height;
    uint32_t states;    // MD_SURFACE_STATE_* bitmask
    uint32_t serial;    // client must ack by committing
} md_surface_configure_t;

// ── md_buffer opcodes ────────────────────────────────────────────────────
// Buffers wrap shared memory regions for pixel data.  The client sends the
// shmem fd via sendfd() immediately after the create message.

// Client → Server requests
#define MD_BUFFER_CREATE    0   // create buffer (+ sendfd for shmem)
#define MD_BUFFER_DESTROY   1   // destroy buffer

// Server → Client events
#define MD_BUFFER_RELEASE   0   // compositor done reading, client can reuse

// create payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;    // bytes per row
    uint32_t format;    // MD_FORMAT_*
    uint32_t offset;    // byte offset into the shmem fd
    uint32_t _pad;
} md_buffer_create_t;

// ── md_seat opcodes ──────────────────────────────────────────────────────
// Input events delivered to the client that owns the focused surface.

// Server → Client events only (no client→server requests for seat)
#define MD_SEAT_KEY_PRESS       0
#define MD_SEAT_KEY_RELEASE     1
#define MD_SEAT_POINTER_MOVE    2
#define MD_SEAT_POINTER_BUTTON  3
#define MD_SEAT_POINTER_ENTER   4
#define MD_SEAT_POINTER_LEAVE   5

// key_press / key_release payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t keycode;
    uint32_t modifiers;     // shift, ctrl, alt bitmask
} md_seat_key_t;

// Modifier bits
#define MD_MOD_SHIFT    (1u << 0)
#define MD_MOD_CTRL     (1u << 1)
#define MD_MOD_ALT      (1u << 2)
#define MD_MOD_SUPER    (1u << 3)

// pointer_move payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    int32_t x;      // surface-relative
    int32_t y;
} md_seat_pointer_move_t;

// pointer_button payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t button;    // 0=left, 1=right, 2=middle
    uint32_t state;     // 1=pressed, 0=released
} md_seat_pointer_button_t;

// pointer_enter / pointer_leave payload
typedef struct __attribute__((packed)) {
    md_msg_header_t hdr;
    uint32_t surface_id;
    int32_t  x;
    int32_t  y;
} md_seat_pointer_surface_t;

// ── Protocol limits ──────────────────────────────────────────────────────

#define MD_MAX_SURFACES     32      // per client
#define MD_MAX_BUFFERS      64      // per client
#define MD_MAX_CLIENTS      16      // total connections
#define MD_MAX_MSG_SIZE     256     // max single message size
#define MD_TITLE_MAX        63      // max title string length

// ── Socket path ──────────────────────────────────────────────────────────

#define MD_SOCKET_PATH  "/tmp/makadisplay.sock"

// ── Server-side decoration constants ─────────────────────────────────────

#define MD_DECO_TITLEBAR_H  24     // title bar height in pixels
#define MD_DECO_BORDER_W    2      // border width in pixels
#define MD_DECO_BUTTON_W    20     // close/minimize button width
#define MD_DECO_BUTTON_H    20     // close/minimize button height

// Title bar colors (BGRX)
#define MD_COLOR_TITLEBAR_ACTIVE    0x00804020  // dark blue
#define MD_COLOR_TITLEBAR_INACTIVE  0x00404040  // gray
#define MD_COLOR_TITLE_TEXT         0x00FFFFFF  // white
#define MD_COLOR_BORDER             0x00606060  // medium gray
#define MD_COLOR_CLOSE_BTN          0x000000CC  // red (BGRX: CC,00,00)
#define MD_COLOR_CLOSE_BTN_HOVER    0x000000FF  // bright red
#define MD_COLOR_BACKGROUND         0x00302820  // desktop background (dark blue-gray)

// ── Helper: build a header ───────────────────────────────────────────────

static inline md_msg_header_t md_msg(uint32_t obj, uint16_t op, uint16_t sz) {
    md_msg_header_t h;
    h.object_id = obj;
    h.opcode    = op;
    h.size      = sz;
    return h;
}
