// doomgeneric_makaos.c — MakaOS platform layer for doomgeneric.
//
// doomgeneric API:
//   DG_Init()             — called once at startup; initialise display/input.
//   DG_DrawFrame()        — blit DG_ScreenBuffer (320×200 RGBA32) to screen.
//   DG_SleepMs(ms)        — sleep for ms milliseconds.
//   uint32_t DG_GetTicksMs() — ms elapsed since program start.
//   int DG_GetKey(int* pressed, unsigned char* key) — poll key events.
//   void DG_SetWindowTitle(const char* title) — optional, ignored.
//
// Pixel format: DG_ScreenBuffer[y*DOOMGENERIC_RESX + x] = 0xRRGGBBAA
//   where R is in the high byte, A is in the low byte.
// Our framebuffer syscall SYS_FB_BLIT accepts this format directly and
// handles the scale-to-screen and RGBA→BGRX conversion in the kernel.

#include "include/stdio.h"   // must come first (redefines rename)
#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdint.h"
#include "include/stdbool.h"
#include "doomgeneric/doomgeneric/doomgeneric.h"
#include "doomgeneric/doomgeneric/doomkeys.h"
#include "doomgeneric/doomgeneric/d_event.h"

// mouse_event_t mirrors the kernel struct exactly (packed, 5 bytes).
typedef struct {
    int16_t  dx;
    int16_t  dy;
    uint8_t  buttons;
} __attribute__((packed)) mouse_event_t;

#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

// ── fb_blit syscall ───────────────────────────────────────────────────────
// SYS_FB_BLIT = 33; arg1 = pointer to 320×200 RGBA buffer.
static inline void fb_blit(const void* buf) {
    syscall4(SYS_FB_BLIT, (uint64_t)buf, DOOMGENERIC_RESX, DOOMGENERIC_RESY, 0);
}

// ── Keyboard / scancode translation ──────────────────────────────────────
// We open /dev/kbdraw which delivers raw PS/2 set-1 scancodes.
// Scancodes < 0x80 are press events; scancodes >= 0x80 are release events
// (release = press | 0x80).
// We translate PS/2 set-1 scancodes to doomgeneric key codes.

static int s_kbdraw_fd = -1;
static int s_mouse_fd  = -1;

// Ring buffer for pending key events.
#define KEY_BUF 256
static struct { unsigned char pressed; unsigned char doomkey; } s_kevents[KEY_BUF];
static int s_kev_r = 0;  // read index
static int s_kev_w = 0;  // write index

static inline void kev_push(unsigned char pressed, unsigned char doomkey) {
    int next = (s_kev_w + 1) & (KEY_BUF - 1);
    if (next == s_kev_r) return;  // overflow: drop oldest slot (keep newest)
    s_kevents[s_kev_w].pressed = pressed;
    s_kevents[s_kev_w].doomkey = doomkey;
    s_kev_w = next;
}

// PS/2 set-1 scancode → doomgeneric key code.
// doomgeneric key codes are defined in doomkeys.h (included by doomgeneric.h).
// We only map the keys Doom actually uses.
static unsigned char scancode_to_doom(unsigned char sc) {
    switch (sc) {
        // Arrow keys
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        // Ctrl / Shift / Alt
        case 0x1D: return KEY_FIRE;    // Left Ctrl = fire
        case 0x2A: return KEY_RSHIFT;  // Left Shift
        case 0x36: return KEY_RSHIFT;  // Right Shift
        case 0x38: return KEY_LALT;    // Left Alt
        // Escape / Enter / Space / Tab
        case 0x01: return KEY_ESCAPE;
        case 0x1C: return KEY_ENTER;
        case 0x39: return KEY_USE;
        case 0x0F: return KEY_TAB;
        // Backspace
        case 0x0E: return KEY_BACKSPACE;
        // Pause
        case 0x45: return KEY_PAUSE;
        // +/-
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;
        // F1-F12
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        // Numeric pad / extra keys
        case 0x53: return KEY_DEL;
        default:
            // Printable ASCII: scancodes 0x02-0x0D (1-=), 0x10-0x1B (q-]),
            // 0x1E-0x28 (a-'), 0x2C-0x35 (z-/).
            {
                // Simple LUT for the main alpha-numeric block.
                static const unsigned char lut[0x36] = {
                    // 0x00 - 0x0F
                    0,   '1','2','3','4','5','6','7','8','9','0','-','=',0,  0,  0,
                    // 0x10 - 0x1F
                    'q','w','e','r','t','y','u','i','o','p','[',']',0,  0,  'a','s',
                    // 0x20 - 0x2F
                    'd','f','g','h','j','k','l',';','\'','`',0, '\\','z','x','c','v',
                    // 0x30 - 0x35
                    'b','n','m',',','.','/',
                };
                if (sc < sizeof(lut) && lut[sc]) return lut[sc];
            }
            return 0;
    }
}

static void poll_keyboard(void) {
    if (s_kbdraw_fd < 0) return;
    unsigned char buf[64];
    // Non-blocking read: SYS_READ with flag bit 0 set in flags arg.
    // Our read_nonblock wrapper uses syscall4(SYS_READ, fd, buf, len, 1).
    ssize_t n = read_nonblock(s_kbdraw_fd, buf, sizeof(buf));
    if (n <= 0) return;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char sc = buf[i];
        // 0xE0 prefix: extended key — skip prefix, handle next byte as-is.
        if (sc == 0xE0) { /* skip — next byte handled without special mapping */ continue; }
        unsigned char pressed  = (sc & 0x80) ? 0 : 1;
        unsigned char base_sc  = sc & 0x7F;
        unsigned char doomkey  = scancode_to_doom(base_sc);
        if (doomkey) kev_push(pressed, doomkey);
    }
}

// ── Mouse input ───────────────────────────────────────────────────────────
// Drains /dev/mouse and posts ev_mouse events directly to the Doom event
// queue via D_PostEvent.  Doom accumulates mouse motion into its own tic
// buffer, so we post one event per kernel packet rather than batching.
//
// Doom ev_mouse layout:
//   data1: button bitmask (bit0=left, bit1=right, bit2=middle)
//   data2: dx (turn left/right)
//   data3: dy (move forward/backward — we ignore; mouse is turn-only by default)

static void poll_mouse(void) {
    if (s_mouse_fd < 0) return;

    mouse_event_t evts[16];
    ssize_t n_bytes = read_nonblock(s_mouse_fd, evts, sizeof(evts));
    if (n_bytes <= 0) return;

    int n = (int)(n_bytes / (int)sizeof(mouse_event_t));
    for (int i = 0; i < n; i++) {
        event_t dev;
        dev.type  = ev_mouse;
        dev.data1 = 0;
        if (evts[i].buttons & MOUSE_BTN_LEFT)   dev.data1 |= 1;
        if (evts[i].buttons & MOUSE_BTN_RIGHT)  dev.data1 |= 2;
        if (evts[i].buttons & MOUSE_BTN_MIDDLE) dev.data1 |= 4;
        dev.data2 = evts[i].dx;   // horizontal = turn
        dev.data3 = evts[i].dy;   // vertical   = forward/back
        dev.data4 = 0;
        D_PostEvent(&dev);
    }
}

// ── Tick counter ──────────────────────────────────────────────────────────
static unsigned long long s_start_ns = 0;

// ── DG_Init ───────────────────────────────────────────────────────────────
void DG_Init(void) {
    s_start_ns  = clock_ns();
    s_kbdraw_fd = open("/dev/kbdraw", O_RDONLY, 0);
    s_mouse_fd  = open("/dev/mouse",  O_RDONLY, 0);
}

// ── DG_DrawFrame ──────────────────────────────────────────────────────────
void DG_DrawFrame(void) {
    fb_blit(DG_ScreenBuffer);
    poll_keyboard();
    poll_mouse();
}

// ── DG_SleepMs ────────────────────────────────────────────────────────────
void DG_SleepMs(uint32_t ms) {
    timespec_t req;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (unsigned long long)(ms % 1000) * 1000000ULL;
    nanosleep(&req, (timespec_t*)0);
}

// ── DG_GetTicksMs ─────────────────────────────────────────────────────────
uint32_t DG_GetTicksMs(void) {
    unsigned long long now = clock_ns();
    return (uint32_t)((now - s_start_ns) / 1000000ULL);
}

// ── DG_GetKey ─────────────────────────────────────────────────────────────
int DG_GetKey(int* pressed, unsigned char* doomkey) {
    poll_keyboard();
    poll_mouse();
    if (s_kev_r == s_kev_w) return 0;
    *pressed = s_kevents[s_kev_r].pressed;
    *doomkey = s_kevents[s_kev_r].doomkey;
    s_kev_r = (s_kev_r + 1) & (KEY_BUF - 1);
    return 1;
}

// ── DG_SetWindowTitle ─────────────────────────────────────────────────────
void DG_SetWindowTitle(const char* title) {
    (void)title;  // No window manager — ignore.
}

// ── _start ────────────────────────────────────────────────────────────────
// Called from user/entry.asm.  We pass a minimal argc/argv to doomgeneric.
// Doom will look for doom1.wad relative to cwd (/bin when launched from shell).
int main(void) {
    static char* argv[] = { "doom", "-iwad", "/bin/doom1.wad", "-mb", "16", (char*)0 };
    doomgeneric_Create(5, argv);
    // D_DoomMain returns after first frame setup — drive the game loop ourselves.
    for (;;) {
        doomgeneric_Tick();
    }
}
