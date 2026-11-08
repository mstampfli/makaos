/*
 * sdl3_gl3 -- prove DESKTOP OpenGL (not just GLES2) works through virgl.
 *
 * Requests an OpenGL 3.3 *core* profile context via SDL (which, on Wayland,
 * binds EGL_OPENGL_API -> Mesa -> virgl -> the host GPU).  Logs GL_VERSION /
 * GL_RENDERER / GLSL version so the desktop-GL version virgl exposes is
 * visible on the serial (CONSOLE_SERIAL=1), then clears the window to a
 * cycling colour.  If GL_VERSION reads e.g. "4.6 (Core Profile) Mesa ...
 * virgl", full desktop GL is real -- no GL4ES translation layer needed.
 *
 * Only common (GL/GLES-shared) entry points are used here (glClear,
 * glClearColor, glGetString, glViewport), so it links against the shared
 * glapi dispatch already in libGLESv2; the *context* is desktop GL.
 * Exits on ESC/Q, close, or after ~6s.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <GLES2/gl2.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("sdl3_gl3: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Desktop OpenGL 3.3 core -- NOT ES.  On Wayland SDL turns this into
    // eglBindAPI(EGL_OPENGL_API) + a core-profile context request.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    SDL_Window* win = SDL_CreateWindow("MakaOS desktop GL 3.3 core", 640, 480,
                                       SDL_WINDOW_OPENGL);
    if (!win) {
        SDL_Log("sdl3_gl3: SDL_CreateWindow(OPENGL) failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        SDL_Log("sdl3_gl3: SDL_GL_CreateContext(3.3 core) failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);

    SDL_Log("sdl3_gl3: GL_VENDOR   = %s", (const char*)glGetString(GL_VENDOR));
    SDL_Log("sdl3_gl3: GL_RENDERER = %s", (const char*)glGetString(GL_RENDERER));
    SDL_Log("sdl3_gl3: GL_VERSION  = %s", (const char*)glGetString(GL_VERSION));
    SDL_Log("sdl3_gl3: GLSL        = %s", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));

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

        glClearColor(b, r, g, 1.0f);   // different phase than sdl3_gl, to tell them apart
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);

        if (t > 6000) running = 0;
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
