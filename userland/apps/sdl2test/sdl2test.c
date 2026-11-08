/*
 * sdl2test -- proves the SDL2 -> sdl2-compat -> dlopen(libSDL3) -> SDL3 bridge.
 * A plain SDL2 program (the API DarkPlaces/Xonotic use): it links libSDL2.a
 * (sdl2-compat), whose SDL_Init loads libSDL3.so.0 at runtime via raw
 * dlopen+dlsym and forwards every call to SDL3.  Headless: SDL_INIT_EVENTS
 * needs no display.
 *
 * Result via the PF-KILL sentinel (headless serial can't see stdout).  Base
 * 0x5D200000.  Full pass => CR2 = 0x000000005D20001F.
 *
 *   bit0  SDL_Init(SDL_INIT_EVENTS)==0  -- sdl2-compat dlopen'd libSDL3.so.0,
 *         filled its ~hundreds-entry SDL3 jump table by dlsym, and SDL3_Init ran
 *   bit1  SDL_GetVersion reports the SDL2 API version (major==2) through the shim
 *   bit2  SDL_WasInit(SDL_INIT_EVENTS) confirms the subsystem is live in SDL3
 *   bit3  SDL_GetError returned a non-NULL string (SDL2 error buffer works)
 *   bit4  SDL_Quit ran cleanly
 */
#include <SDL2/SDL.h>

int main(void) {
    unsigned long r = 0x5D200000UL;

    if (SDL_Init(SDL_INIT_EVENTS) == 0) {
        r |= 0x01UL;

        SDL_version v;
        SDL_GetVersion(&v);
        if (v.major == 2) r |= 0x02UL;

        if (SDL_WasInit(SDL_INIT_EVENTS) & SDL_INIT_EVENTS) r |= 0x04UL;

        if (SDL_GetError() != NULL) r |= 0x08UL;

        SDL_Quit();
        r |= 0x10UL;
    }

    *(volatile unsigned char*)r = 0;   /* PF-KILL: CR2=0x5D20001F on full pass */
    return 0;
}
