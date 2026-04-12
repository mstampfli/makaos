// MakaDisplay demo client — draws a colored rectangle in a window

#include "libdisplay.h"

static int g_running = 1;
static md_display_t*       g_dpy;
static md_client_surface_t* g_surf;
static md_client_buffer_t*  g_buf;
static uint32_t g_w = 300, g_h = 200;

static void paint_gradient(md_client_buffer_t* buf, void* ud) {
    (void)ud;
    uint32_t w = md_buffer_width(buf);
    uint32_t h = md_buffer_height(buf);
    uint32_t* pixels = md_buffer_data(buf);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)(x * 255 / (w ? w : 1));
            uint8_t gg = (uint8_t)(y * 255 / (h ? h : 1));
            uint8_t b = 128;
            pixels[y * w + x] = (uint32_t)b | ((uint32_t)gg << 8) | ((uint32_t)r << 16);
        }
    }
}

static void render(void) {
    paint_gradient(g_buf, 0);
    md_surface_attach(g_surf, g_buf);
    md_surface_damage(g_surf, 0, 0, g_w, g_h);
    md_surface_commit(g_surf);
}

static void on_configure(md_client_surface_t* surf, uint32_t width,
                         uint32_t height, uint32_t states) {
    (void)surf; (void)states;
    if (width == 0 || height == 0) return;
    if (width == g_w && height == g_h) return;
    g_w = width;
    g_h = height;
    if (md_surface_resize_commit(g_surf, &g_buf, g_w, g_h,
                                 paint_gradient, 0) < 0) {
        g_running = 0;
    }
}

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

    g_dpy = md_display_connect();
    if (!g_dpy) {
        print("demo_client: failed to connect\n");
        return 1;
    }

    print("demo_client: connected\n");

    g_surf = md_surface_create(g_dpy);
    if (!g_surf) {
        print("demo_client: failed to create surface\n");
        md_display_disconnect(g_dpy);
        return 1;
    }

    md_surface_set_title(g_surf, "Demo Window");
    md_surface_on_close(g_surf, on_close);
    md_surface_on_key(g_surf, on_key);
    md_surface_on_configure(g_surf, on_configure);

    g_buf = md_buffer_create(g_dpy, g_w, g_h);
    if (!g_buf) {
        print("demo_client: failed to create buffer\n");
        md_display_disconnect(g_dpy);
        return 1;
    }

    render();

    print("demo_client: window displayed, press ESC or Q to quit\n");

    while (g_running) {
        int r = md_display_dispatch_blocking(g_dpy);
        if (r < 0) break;
    }

    print("demo_client: exiting\n");
    if (g_buf) md_buffer_destroy(g_buf);
    md_surface_destroy(g_surf);
    md_display_disconnect(g_dpy);
    return 0;
}
