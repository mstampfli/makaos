// doomgeneric_makaos.c — MakaOS platform layer for doomgeneric.
//
// Uses the MakaDisplay compositor for rendering and input.
// Doom renders into DG_ScreenBuffer (320x200 RGBA32: 0xRRGGBBAA).
// We convert to BGRX8888 and blit into a shared-memory surface buffer.
// Input arrives via compositor seat key/pointer events.

#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdint.h"
#include "include/stdbool.h"
#include "doomgeneric/doomgeneric/doomgeneric.h"
#include "doomgeneric/doomgeneric/doomkeys.h"
#include "doomgeneric/doomgeneric/d_event.h"
#include "../display/libdisplay.h"

// ── Display server state ─────────────────────────────────────────────────

static md_display_t*        s_dpy  = 0;
static md_client_surface_t* s_surf = 0;
static md_client_buffer_t*  s_buf  = 0;

// ── Keyboard ring buffer ─────────────────────────────────────────────────

#define KEY_BUF 256
static struct { unsigned char pressed; unsigned char doomkey; } s_kevents[KEY_BUF];
static int s_kev_r = 0;
static int s_kev_w = 0;

static inline void kev_push(unsigned char pressed, unsigned char doomkey) {
    int next = (s_kev_w + 1) & (KEY_BUF - 1);
    if (next == s_kev_r) return;
    s_kevents[s_kev_w].pressed = pressed;
    s_kevents[s_kev_w].doomkey = doomkey;
    s_kev_w = next;
}

// ── Evdev keycode → Doom keycode translation ─────────────────────────────
// The compositor sends Linux evdev keycodes (KEY_* from input_core.h).
// These happen to match PS/2 set-1 scan codes for the main block.

// Evdev key codes (from kernel/drivers/input/input_core.h)
#define EV_KEY_ESC         1
#define EV_KEY_1           2
#define EV_KEY_2           3
#define EV_KEY_3           4
#define EV_KEY_4           5
#define EV_KEY_5           6
#define EV_KEY_6           7
#define EV_KEY_7           8
#define EV_KEY_8           9
#define EV_KEY_9           10
#define EV_KEY_0           11
#define EV_KEY_MINUS       12
#define EV_KEY_EQUAL       13
#define EV_KEY_BACKSPACE   14
#define EV_KEY_TAB         15
#define EV_KEY_Q           16
#define EV_KEY_W           17
#define EV_KEY_E           18
#define EV_KEY_R           19
#define EV_KEY_T           20
#define EV_KEY_Y           21
#define EV_KEY_U           22
#define EV_KEY_I           23
#define EV_KEY_O           24
#define EV_KEY_P           25
#define EV_KEY_LEFTBRACE   26
#define EV_KEY_RIGHTBRACE  27
#define EV_KEY_ENTER       28
#define EV_KEY_LEFTCTRL    29
#define EV_KEY_A           30
#define EV_KEY_S           31
#define EV_KEY_D           32
#define EV_KEY_F           33
#define EV_KEY_G           34
#define EV_KEY_H           35
#define EV_KEY_J           36
#define EV_KEY_K           37
#define EV_KEY_L           38
#define EV_KEY_SEMICOLON   39
#define EV_KEY_APOSTROPHE  40
#define EV_KEY_GRAVE       41
#define EV_KEY_LEFTSHIFT   42
#define EV_KEY_BACKSLASH   43
#define EV_KEY_Z           44
#define EV_KEY_X           45
#define EV_KEY_C           46
#define EV_KEY_V           47
#define EV_KEY_B           48
#define EV_KEY_N           49
#define EV_KEY_M           50
#define EV_KEY_COMMA       51
#define EV_KEY_DOT         52
#define EV_KEY_SLASH       53
#define EV_KEY_RIGHTSHIFT  54
#define EV_KEY_LEFTALT     56
#define EV_KEY_SPACE       57
#define EV_KEY_F1          59
#define EV_KEY_F2          60
#define EV_KEY_F3          61
#define EV_KEY_F4          62
#define EV_KEY_F5          63
#define EV_KEY_F6          64
#define EV_KEY_F7          65
#define EV_KEY_F8          66
#define EV_KEY_F9          67
#define EV_KEY_F10         68
#define EV_KEY_NUMLOCK     69
#define EV_KEY_F11         87
#define EV_KEY_F12         88
#define EV_KEY_RIGHTCTRL   97
#define EV_KEY_RIGHTALT    100
#define EV_KEY_UP          103
#define EV_KEY_LEFT        105
#define EV_KEY_RIGHT       106
#define EV_KEY_DOWN        108
#define EV_KEY_DELETE      111
#define EV_KEY_PAUSE       119

static unsigned char evdev_to_doom(uint32_t code) {
    switch (code) {
    case EV_KEY_UP:         return KEY_UPARROW;
    case EV_KEY_DOWN:       return KEY_DOWNARROW;
    case EV_KEY_LEFT:       return KEY_LEFTARROW;
    case EV_KEY_RIGHT:      return KEY_RIGHTARROW;
    case EV_KEY_LEFTCTRL:
    case EV_KEY_RIGHTCTRL:  return KEY_FIRE;
    case EV_KEY_LEFTSHIFT:
    case EV_KEY_RIGHTSHIFT: return KEY_RSHIFT;
    case EV_KEY_LEFTALT:
    case EV_KEY_RIGHTALT:   return KEY_LALT;
    case EV_KEY_ESC:        return KEY_ESCAPE;
    case EV_KEY_ENTER:      return KEY_ENTER;
    case EV_KEY_SPACE:      return KEY_USE;
    case EV_KEY_TAB:        return KEY_TAB;
    case EV_KEY_BACKSPACE:  return KEY_BACKSPACE;
    case EV_KEY_PAUSE:      return KEY_PAUSE;
    case EV_KEY_MINUS:      return KEY_MINUS;
    case EV_KEY_EQUAL:      return KEY_EQUALS;
    case EV_KEY_DELETE:      return KEY_DEL;
    case EV_KEY_F1:         return KEY_F1;
    case EV_KEY_F2:         return KEY_F2;
    case EV_KEY_F3:         return KEY_F3;
    case EV_KEY_F4:         return KEY_F4;
    case EV_KEY_F5:         return KEY_F5;
    case EV_KEY_F6:         return KEY_F6;
    case EV_KEY_F7:         return KEY_F7;
    case EV_KEY_F8:         return KEY_F8;
    case EV_KEY_F9:         return KEY_F9;
    case EV_KEY_F10:        return KEY_F10;
    case EV_KEY_F11:        return KEY_F11;
    case EV_KEY_F12:        return KEY_F12;
    default: break;
    }
    // Alphanumeric: evdev codes 2-13 = '1'-'=', 16-25 = 'q'-'p', etc.
    // Map to ASCII lowercase for Doom.
    {
        static const unsigned char lut[54] = {
            0,   0,  '1','2','3','4','5','6','7','8','9','0','-','=',
            0,   0,  'q','w','e','r','t','y','u','i','o','p','[',']',
            0,   0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
            0, '\\', 'z','x','c','v','b','n','m',',','.','/',
        };
        if (code < sizeof(lut) && lut[code]) return lut[code];
    }
    return 0;
}

// ── Event callbacks from compositor ──────────────────────────────────────

static void on_key(md_client_surface_t* surf, uint32_t keycode,
                   uint32_t modifiers, int pressed) {
    (void)surf; (void)modifiers;
    unsigned char dk = evdev_to_doom(keycode);
    if (dk) kev_push(pressed ? 1 : 0, dk);
}

// Mouse state: Doom wants dx (turn) and button bitmask per ev_mouse event.
// The compositor sends surface-relative coordinates, but Doom needs deltas.
// We track last position and compute deltas.
static int32_t s_last_mx = -1, s_last_my = -1;
static uint8_t s_mouse_btn = 0;

static void on_pointer_move(md_client_surface_t* surf, int32_t x, int32_t y) {
    (void)surf;
    if (s_last_mx < 0) { s_last_mx = x; s_last_my = y; return; }
    int32_t dx = x - s_last_mx;
    int32_t dy = y - s_last_my;
    s_last_mx = x;
    s_last_my = y;
    if (dx == 0 && dy == 0) return;

    event_t dev;
    dev.type  = ev_mouse;
    dev.data1 = (int)s_mouse_btn;
    dev.data2 = dx;
    dev.data3 = dy;
    dev.data4 = 0;
    D_PostEvent(&dev);
}

static void on_pointer_button(md_client_surface_t* surf, uint32_t button, int pressed) {
    (void)surf;
    // button: 0=left, 1=right, 2=middle
    uint8_t mask = (uint8_t)(1u << button);
    if (pressed) s_mouse_btn |= mask;
    else         s_mouse_btn &= ~mask;

    // Post a mouse event with updated buttons (no motion)
    event_t dev;
    dev.type  = ev_mouse;
    dev.data1 = (int)s_mouse_btn;
    dev.data2 = 0;
    dev.data3 = 0;
    dev.data4 = 0;
    D_PostEvent(&dev);
}

static void on_close(md_client_surface_t* surf) {
    (void)surf;
    _exit(0);
}

// ── Tick counter ─────────────────────────────────────────────────────────
static unsigned long long s_start_ns = 0;

// ── RGBA→BGRX conversion and blit ───────────────────────────────────────
// Doom: pixel = 0xRRGGBBAA  (R in bits 31:24, A in bits 7:0)
// Display: BGRX8888 = 0x00RRGGBB in memory as B,G,R,X
// So: B = (pixel >> 16) & 0xFF ... wait, let's be precise.
//
// Doom pixel: 0xRRGGBBAA
//   R = (px >> 24) & 0xFF
//   G = (px >> 16) & 0xFF
//   B = (px >> 8)  & 0xFF
//   A = px & 0xFF (ignored)
//
// BGRX in uint32_t (little-endian): stored as B,G,R,X in memory
//   = (X << 24) | (R << 16) | (G << 8) | B
//   = (R << 16) | (G << 8) | B  (X=0)
//
// So: out = ((px >> 24) << 16) | ((px >> 16) & 0xFF) << 8) | ((px >> 8) & 0xFF)

static void blit_to_buffer(void) {
    if (!s_buf) return;
    uint32_t* dst = md_buffer_data(s_buf);
    uint32_t* src = DG_ScreenBuffer;
    uint32_t count = DOOMGENERIC_RESX * DOOMGENERIC_RESY;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t px = src[i];
        uint32_t r = (px >> 24) & 0xFF;
        uint32_t g = (px >> 16) & 0xFF;
        uint32_t b = (px >> 8)  & 0xFF;
        dst[i] = (r << 16) | (g << 8) | b;
    }
}

// ── DG_Init ──────────────────────────────────────────────────────────────
void DG_Init(void) {
    s_start_ns = clock_ns();

    // Connect to display server
    s_dpy = md_display_connect();
    if (!s_dpy) {
        // Fallback: try a few times with short delays (compositor may still be starting)
        for (int i = 0; i < 20 && !s_dpy; i++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000LL }; // 100ms
            nanosleep(&ts, 0);
            s_dpy = md_display_connect();
        }
        if (!s_dpy) _exit(1);
    }

    // Create surface (window)
    s_surf = md_surface_create(s_dpy);
    if (!s_surf) _exit(1);
    md_surface_set_title(s_surf, "DOOM");

    // Create pixel buffer at Doom's native resolution
    s_buf = md_buffer_create(s_dpy, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    if (!s_buf) _exit(1);

    // Register event handlers
    md_surface_on_key(s_surf, on_key);
    md_surface_on_pointer_move(s_surf, on_pointer_move);
    md_surface_on_pointer_button(s_surf, on_pointer_button);
    md_surface_on_close(s_surf, on_close);
}

// ── DG_DrawFrame ─────────────────────────────────────────────────────────
void DG_DrawFrame(void) {
    // Convert RGBA→BGRX into shared memory buffer
    blit_to_buffer();

    // Submit to compositor
    md_surface_attach(s_surf, s_buf);
    md_surface_damage(s_surf, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    md_surface_commit(s_surf);

    // Drain pending compositor events (non-blocking)
    md_display_dispatch(s_dpy);
}

// ── DG_SleepMs ───────────────────────────────────────────────────────────
void DG_SleepMs(uint32_t ms) {
    struct timespec req;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (int64_t)((ms % 1000) * 1000000ULL);
    nanosleep(&req, NULL);
}

// ── DG_GetTicksMs ────────────────────────────────────────────────────────
uint32_t DG_GetTicksMs(void) {
    unsigned long long now = clock_ns();
    return (uint32_t)((now - s_start_ns) / 1000000ULL);
}

// ── DG_GetKey ────────────────────────────────────────────────────────────
int DG_GetKey(int* pressed, unsigned char* doomkey) {
    // Drain compositor events to pick up any new key presses
    md_display_dispatch(s_dpy);
    if (s_kev_r == s_kev_w) return 0;
    *pressed = s_kevents[s_kev_r].pressed;
    *doomkey = s_kevents[s_kev_r].doomkey;
    s_kev_r = (s_kev_r + 1) & (KEY_BUF - 1);
    return 1;
}

// ── DG_SetWindowTitle ────────────────────────────────────────────────────
void DG_SetWindowTitle(const char* title) {
    if (s_surf) md_surface_set_title(s_surf, title);
}

// ── main ─────────────────────────────────────────────────────────────────
int main(void) {
    static char* argv[] = { "doom", "-iwad", "/bin/doom1.wad", "-mb", "16", (char*)0 };
    doomgeneric_Create(5, argv);
    for (;;) {
        doomgeneric_Tick();
    }
}
