// glfwtest.c -- GLFW-Wayland smoke test on MakaOS.
//
// Proves the ported GLFW's Wayland backend end to end: connect to the sway
// compositor, create a window + EGL/GL context through Mesa/virgl, and render a
// few frames.  This is the piece Minecraft's LWJGL needs, so getting a real GL
// context + version string here de-risks the whole path.
#include <GLFW/glfw3.h>
#include <GLES2/gl2.h>
#include <stdio.h>

static void err_cb(int code, const char* desc) {
    printf("[glfwtest] GLFW error %d: %s\n", code, desc ? desc : "(null)");
}

int main(void) {
    glfwSetErrorCallback(err_cb);
    if (!glfwInit()) { printf("[glfwtest] glfwInit FAILED\n"); return 1; }
    printf("[glfwtest] glfwInit OK (platform=%d)\n", glfwGetPlatform());

    // A GLES2 context via EGL -- the safest path through Mesa/virgl. (Desktop GL
    // 4.2 Core is also available; DarkPlaces uses it.)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* win = glfwCreateWindow(640, 480, "MakaOS GLFW smoke test", NULL, NULL);
    if (!win) { printf("[glfwtest] glfwCreateWindow FAILED\n"); glfwTerminate(); return 1; }
    printf("[glfwtest] glfwCreateWindow OK\n");

    glfwMakeContextCurrent(win);
    printf("[glfwtest] GL_VERSION  = %s\n", (const char*)glGetString(GL_VERSION));
    printf("[glfwtest] GL_RENDERER = %s\n", (const char*)glGetString(GL_RENDERER));

    int frames = 0;
    for (int i = 0; i < 20 && !glfwWindowShouldClose(win); i++) {
        float t = (float)i / 20.0f;
        glClearColor(0.2f, t, 0.8f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(win);
        glfwPollEvents();
        frames++;
    }
    printf("[glfwtest] rendered %d frames -- GLFW-Wayland SMOKE TEST OK\n", frames);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
