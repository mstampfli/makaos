/*
 * dplaunch -- launch DarkPlaces headless.  autologin can only exec a bare path
 * (no args, no env), so this tiny wrapper forces SDL's "offscreen" video driver
 * (a surfaceless EGL context via Mesa -- no wayland compositor needed, falls
 * back to swrast without a GPU) and hands DarkPlaces its basedir, then execs it.
 * setenv writes the current environ; execv passes that environ through.
 */
extern char* getenv(const char*);
extern int   setenv(const char*, const char*, int);
extern int   execv(const char*, char* const[]);

int main(void) {
    /* Under a compositor (sway sets WAYLAND_DISPLAY) let SDL pick the wayland
     * driver so the game is VISIBLE in a window.  Headless (no compositor) force
     * the offscreen surfaceless-EGL driver so it still renders (into an off-
     * screen buffer) with no display. */
    if (!getenv("WAYLAND_DISPLAY"))
        setenv("SDL_VIDEODRIVER", "offscreen", 1);
    char* argv[] = { "/bin/darkplaces", "-basedir", "/root", (char*)0 };
    execv("/bin/darkplaces", argv);
    return 1;   /* only reached if execv failed */
}
