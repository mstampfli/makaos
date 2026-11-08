// gl_glx_shim.c - GLX proc-loader shim over Mesa's EGL, for the self-contained
// libglfw.so that LWJGL (and therefore Minecraft) loads.
//
// LWJGL's org.lwjgl.opengl.GL resolves every GL entry point through
// glXGetProcAddress (the GLX proc loader), falling back to plain dlsym only if
// that symbol is absent. MakaOS's Mesa is built with -Dglx=disabled
// -Dglvnd=false, so there is no GLX and no flat desktop-GL symbols
// (glPolygonMode, glDrawBuffer, ...): the ONLY way to obtain the full desktop
// GL 4.x entry-point set is Mesa's eglGetProcAddress, which walks the shared
// glapi mapi table (built with -Dopengl=true) and returns a dispatch stub bound
// to the current context. This is exactly how the DarkPlaces port reached GL
// 4.2 Core (SDL -> eglGetProcAddress).
//
// So we expose glXGetProcAddress / glXGetProcAddressARB as thin forwarders to
// eglGetProcAddress. Both live in the SAME shared object (libglfw.so whole-
// archives libEGL), so the returned GL stubs dispatch through the very glapi
// instance in which GLFW made the context current -- no second Mesa state.

typedef void (*__GLproc)(void);

// Mesa's EGL proc loader, whole-archived into this same .so.
extern __GLproc eglGetProcAddress(const char *procname);

__GLproc glXGetProcAddress(const unsigned char *procName)
{
    return eglGetProcAddress((const char *)procName);
}

__GLproc glXGetProcAddressARB(const unsigned char *procName)
{
    return eglGetProcAddress((const char *)procName);
}
