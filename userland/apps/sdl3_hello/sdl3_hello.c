/*
 * sdl3_hello — smallest-possible SDL3 app to prove end-to-end the
 * libSDL3 port works on the Wayland backend under dwl.
 *
 * Opens a 640x480 window.  On each frame, fills the window with a
 * colour that cycles through the HSL hue range so there's obvious
 * visible motion.  Exits on ESC, Q, window-close, or after ~5s.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("MakaOS SDL3 hello", 640, 480, 0);
    if (!win) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* r = SDL_CreateRenderer(win, NULL);
    if (!r) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

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

        // Cycle R/G/B so motion is obvious on screen.
        Uint64 t = SDL_GetTicks() - start;
        Uint8  rr = (Uint8)((t / 4) & 0xFF);
        Uint8  gg = (Uint8)(((t / 4) + 85) & 0xFF);
        Uint8  bb = (Uint8)(((t / 4) + 170) & 0xFF);

        SDL_SetRenderDrawColor(r, rr, gg, bb, 255);
        SDL_RenderClear(r);
        SDL_RenderPresent(r);

        // Cap the main loop roughly.  The compositor will drive us
        // with frame callbacks once we hook them up properly.
        SDL_Delay(16);

        if (t > 5000) running = 0;
    }

    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
