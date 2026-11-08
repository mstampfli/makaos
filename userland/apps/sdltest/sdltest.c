/*
 * sdltest -- the dynamic loader's first real-world proof: dlopen() the actual
 * 83 MB libSDL3.so (SDL3 + the whole Mesa GL stack + wayland + a PIC C++
 * runtime, ~26k relocations, general-dynamic TLS incl. a cross-module errno
 * import), dlsym real SDL entry points, and call them -- all headless, no
 * window or GPU (SDL_GetVersion / SDL_GetError are pure queries).
 *
 * RTLD_NOW means a successful dlopen resolved EVERY one of the .so's relocs and
 * all 293 strong libc imports against the exe -- so bit0 alone exercises far
 * more of the loader than the synthetic dltest ever could.
 *
 * Result is signalled the same way as dltest: a sentinel page fault whose low
 * bits encode the checks (headless serial can't see stdout).  Base 0x5D100000
 * has clean low bits.  Full pass => CR2 = 0x000000005D10001F.
 *
 *   bit0  dlopen("/lib/libSDL3.so", RTLD_NOW) succeeded (all relocs resolved)
 *   bit1  dlsym found SDL_GetVersion
 *   bit2  SDL_GetVersion() returned a sane SDL3 version (3000000..3999999)
 *   bit3  dlsym found SDL_GetError and it returned a non-NULL C string
 *   bit4  dlclose returned 0
 */
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int   dlclose(void*);

int main(void) {
    unsigned long r = 0x5D100000UL;

    void* h = dlopen("libSDL3.so.0", 2 /*RTLD_NOW*/);   /* bare soname -> /lib (standard SDL3 runtime name) */
    if (h) {
        r |= 0x01UL;

        int (*get_version)(void) = (int (*)(void))dlsym(h, "SDL_GetVersion");
        if (get_version) {
            r |= 0x02UL;
            int v = get_version();
            if (v >= 3000000 && v < 4000000) r |= 0x04UL;   /* SDL 3.x */
        }

        const char* (*get_error)(void) = (const char* (*)(void))dlsym(h, "SDL_GetError");
        if (get_error) {
            const char* e = get_error();                    /* touches SDL's TLS error slot */
            if (e) r |= 0x08UL;
        }

        if (dlclose(h) == 0) r |= 0x10UL;
    }

    *(volatile unsigned char*)r = 0;   /* PF-KILL: CR2 = 0x5D10001F on full pass */
    return 0;
}
