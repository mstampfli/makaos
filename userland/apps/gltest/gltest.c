/*
 * gltest -- first light through the full ported GL stack (docs/VIRGL_BRINGUP.md
 * phase 3c).  Links the STATIC Mesa build (libEGL/libGLESv2/libgbm +
 * libgallium_dri with the virgl gallium driver) and drives real hardware GL:
 *
 *   open(/dev/dri/renderD128) -> gbm_create_device -> eglGetPlatformDisplay(GBM)
 *   -> eglInitialize -> GLES2 context -> surfaceless make-current -> render to
 *   an offscreen RGBA8 FBO: glClearColor(magenta) + glClear -> glReadPixels,
 *
 * then checks the pixels are exactly magenta.  If they are, the whole path
 * worked: EGL -> gallium virgl winsys -> our virtio-gpu render node -> the host
 * GPU rendered it -> we read it back.  GL_VENDOR/RENDERER/VERSION are printed
 * (the RENDERER string is the host's virgl string).
 *
 * Verdict goes to the kernel serial log the same way virgltest signals it: an
 * unknown-ioctl request number the drm layer pr_warns (0x600D600D = PASS,
 * 0x0BADBAD0 = FAIL), since app stdout lands on the VGA console.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#define W 64u
#define H 64u

/* Failure is signalled to the kernel serial log via an unknown-ioctl req number
   0xFA11<step> (the drm layer pr_warns it), since app stdout goes to VGA. Each
   fail() site has a distinct step so a headless run shows where it stopped. */
static int g_drmfd = -1;
static int fail(unsigned step, const char* msg) {
    if (g_drmfd >= 0) ioctl(g_drmfd, 0xFA110000UL | (step & 0xFFFFu), (void*)0);
    printf("gltest: FAIL[%u] %s\n", step, msg);
    return 1;
}

/* EGL debug callback: emit the message text via 0xE8<3 chars> sentinels so a
   headless run shows WHY EGL failed (the internal _eglError message string). */
static void egl_dbg(EGLenum error, const char* command, EGLint mtype,
                    EGLLabelKHR t, EGLLabelKHR o, const char* msg) {
    (void)error; (void)command; (void)mtype; (void)t; (void)o;
    if (!msg || g_drmfd < 0) return;
    for (unsigned i = 0; msg[i] && i < 45u; i += 3u) {
        unsigned c0 = (unsigned char)msg[i];
        unsigned c1 = msg[i + 1] ? (unsigned char)msg[i + 1] : 0;
        unsigned c2 = (msg[i + 1] && msg[i + 2]) ? (unsigned char)msg[i + 2] : 0;
        ioctl(g_drmfd, 0xE8000000UL | (c0 << 16) | (c1 << 8) | c2, (void*)0);
    }
    ioctl(g_drmfd, 0xE8FFFFFFUL, (void*)0);   /* message boundary */
}

int main(void) {
    int drmfd = open("/dev/dri/renderD128", O_RDWR, 0);
    if (drmfd < 0) { printf("gltest: FAIL open renderD128\n"); return 1; }
    g_drmfd = drmfd;

    PFNEGLDEBUGMESSAGECONTROLKHRPROC dbgctl =
        (PFNEGLDEBUGMESSAGECONTROLKHRPROC)eglGetProcAddress("eglDebugMessageControlKHR");
    if (dbgctl) {
        EGLAttrib attrs[] = { EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE,
                              EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE,
                              EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_NONE };
        dbgctl(egl_dbg, attrs);
    }

    struct gbm_device* gbm = gbm_create_device(drmfd);
    if (!gbm) return fail(1, "gbm_create_device");

    /* EGL display on the GBM (DRM) platform. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getPlatformDisplay
        ? getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL)
        : eglGetDisplay((EGLNativeDisplayType)gbm);
    if (dpy == EGL_NO_DISPLAY) return fail(2, "eglGetPlatformDisplay");

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        ioctl(drmfd, 0xE6100000UL | (eglGetError() & 0xFFFFu), (void*)0);  /* EGL error code */
        return fail(3, "eglInitialize");
    }
    printf("gltest: EGL %d.%d  vendor=%s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail(4, "eglBindAPI");

    // We render surfaceless to an FBO, so the surface type is irrelevant to us;
    // GBM/virgl expose window configs, so ask for EGL_WINDOW_BIT (the default)
    // rather than pbuffer, and don't over-constrain the color sizes.
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &config, 1, &n) || n < 1)
        return fail(5, "eglChooseConfig");

    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) return fail(6, "eglCreateContext");

    /* Surfaceless: render to an FBO, no window/pbuffer needed. */
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return fail(7, "eglMakeCurrent(surfaceless)");

    printf("gltest: GL_VENDOR=%s\n", (const char*)glGetString(GL_VENDOR));
    printf("gltest: GL_RENDERER=%s\n", (const char*)glGetString(GL_RENDERER));
    printf("gltest: GL_VERSION=%s\n", (const char*)glGetString(GL_VERSION));

    /* Offscreen RGBA8 render target. */
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail(8, "FBO incomplete");

    ioctl(drmfd, 0xC1000001UL, (void*)0);   /* progress: FBO complete, about to clear */
    glViewport(0, 0, W, H);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);   /* magenta */
    glClear(GL_COLOR_BUFFER_BIT);
    ioctl(drmfd, 0xC1000002UL, (void*)0);   /* progress: cleared, about to finish */
    glFinish();
    ioctl(drmfd, 0xC1000003UL, (void*)0);   /* progress: finished, about to readpixels */

    unsigned char* px = (unsigned char*)malloc(W * H * 4);
    if (!px) return fail(9, "malloc");
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    ioctl(drmfd, 0xC1000004UL, (void*)0);   /* progress: readpixels done */

    GLenum err = glGetError();
    unsigned bad = 0, first = ((unsigned)px[0]) | ((unsigned)px[1] << 8) |
                              ((unsigned)px[2] << 16) | ((unsigned)px[3] << 24);
    for (unsigned i = 0; i < W * H; i++) {
        const unsigned char* p = px + i * 4;
        if (!(p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255)) bad++;
    }

    int pass = (err == GL_NO_ERROR && bad == 0);
    printf("gltest: glError=0x%x  first=%08x  bad=%u/%u -> %s\n",
           err, first, bad, W * H, pass ? "PASS" : "FAIL");

    /* Verdict to the kernel serial log via an unknown-ioctl req number. */
    ioctl(drmfd, pass ? 0x600D600DUL : 0x0BADBAD0UL, (void*)0);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(drmfd);
    return pass ? 0 : 1;
}
