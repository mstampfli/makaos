/*
 * sdl3_gl -- the Phase 2 make-or-break smoke test for hardware GL on MakaOS.
 *
 * Opens an SDL3 window with an OpenGL ES 2.0 context (via the Wayland-EGL
 * client path -> Mesa -> virgl -> host GPU) and, every frame, clears the GL
 * framebuffer to a cycling colour and swaps.  If sway composites this window,
 * then a client rendering GLES2 shares its buffer to the compositor as a
 * dma-buf and gets scanned out -- i.e. client-side hardware GL works end to
 * end, and any GL game becomes portable.
 *
 * The GL strings are logged to stderr (mirrored to serial with
 * CONSOLE_SERIAL=1) so the renderer can be confirmed as "virgl" headlessly.
 * Exits on ESC/Q, window close, or after ~6s.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <GLES2/gl2.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("sdl3_gl: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Ask for a GLES 2.0 context -- that's what Mesa/virgl exposes here.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    SDL_Window* win = SDL_CreateWindow("MakaOS GLES2 smoke test", 640, 480,
                                       SDL_WINDOW_OPENGL);
    if (!win) {
        SDL_Log("sdl3_gl: SDL_CreateWindow(OPENGL) failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        SDL_Log("sdl3_gl: SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);

    // These prove which stack we landed on -- expect a virgl/Mesa renderer.
    SDL_Log("sdl3_gl: GL_VENDOR   = %s", (const char*)glGetString(GL_VENDOR));
    SDL_Log("sdl3_gl: GL_RENDERER = %s", (const char*)glGetString(GL_RENDERER));
    SDL_Log("sdl3_gl: GL_VERSION  = %s", (const char*)glGetString(GL_VERSION));

    Uint64 start = SDL_GetTicks();
    int    running = 1;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE || k == SDLK_Q) running = 0;
            }
        }

        Uint64 t = SDL_GetTicks() - start;
        float r = (float)((t / 4) & 0xFF) / 255.0f;
        float g = (float)(((t / 4) + 85) & 0xFF) / 255.0f;
        float b = (float)(((t / 4) + 170) & 0xFF) / 255.0f;

        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);

        if (t > 6000) running = 0;
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
