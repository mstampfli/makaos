// MakaDisplay demo client — draws a colored rectangle in a window

#include "libdisplay.h"

static int g_running = 1;

static void on_close(md_client_surface_t* surf) {
    (void)surf;
    g_running = 0;
}

static void on_key(md_client_surface_t* surf, uint32_t keycode,
                   uint32_t modifiers, int pressed) {
    (void)modifiers;
    (void)pressed;
    (void)surf;
    // 'q' scancode = 0x10, ESC = 0x01
    if (keycode == 0x01 || keycode == 0x10) {
        g_running = 0;
    }
}

static void print(const char* s) {
    uint32_t len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    print("demo_client: connecting to display server...\n");

    md_display_t* dpy = md_display_connect();
    if (!dpy) {
        print("demo_client: failed to connect\n");
        return 1;
    }

    print("demo_client: connected\n");

    // Create a surface (window)
    md_client_surface_t* surf = md_surface_create(dpy);
    if (!surf) {
        print("demo_client: failed to create surface\n");
        md_display_disconnect(dpy);
        return 1;
    }

    md_surface_set_title(surf, "Demo Window");
    md_surface_on_close(surf, on_close);
    md_surface_on_key(surf, on_key);

    // Create a pixel buffer (300x200)
    uint32_t w = 300, h = 200;
    md_client_buffer_t* buf = md_buffer_create(dpy, w, h);
    if (!buf) {
        print("demo_client: failed to create buffer\n");
        md_display_disconnect(dpy);
        return 1;
    }

    // Draw a gradient pattern
    uint32_t* pixels = md_buffer_data(buf);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)(x * 255 / w);
            uint8_t g = (uint8_t)(y * 255 / h);
            uint8_t b = 128;
            // BGRX: B | (G<<8) | (R<<16)
            pixels[y * w + x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
        }
    }

    // Attach and commit
    md_surface_attach(surf, buf);
    md_surface_damage(surf, 0, 0, w, h);
    md_surface_commit(surf);

    print("demo_client: window displayed, press ESC or Q to quit\n");

    // Event loop
    while (g_running) {
        int r = md_display_dispatch_blocking(dpy);
        if (r < 0) break;
    }

    print("demo_client: exiting\n");
    md_buffer_destroy(buf);
    md_surface_destroy(surf);
    md_display_disconnect(dpy);
    return 0;
}
