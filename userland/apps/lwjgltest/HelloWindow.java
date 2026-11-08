import static org.lwjgl.glfw.GLFW.*;
import static org.lwjgl.opengl.GL11.*;
import static org.lwjgl.system.MemoryUtil.NULL;
import org.lwjgl.opengl.GL;

// Minimal LWJGL 3 window: the exact stack Minecraft uses (LWJGL -> GLFW ->
// GL context). Proving this on MakaOS validates JVM -> LWJGL -> libglfw.so
// (our Wayland GLFW) -> Mesa/virgl end to end.
public class HelloWindow {
    public static void main(String[] args) {
        System.out.println("[lwjgl] loading; GLFW " + glfwGetVersionString());
        if (!glfwInit()) { System.out.println("[lwjgl] glfwInit FAILED"); return; }
        System.out.println("[lwjgl] glfwInit OK");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        long win = glfwCreateWindow(640, 480, "MakaOS LWJGL", NULL, NULL);
        if (win == NULL) { System.out.println("[lwjgl] createWindow FAILED"); glfwTerminate(); return; }
        System.out.println("[lwjgl] createWindow OK");
        glfwMakeContextCurrent(win);
        GL.createCapabilities();
        System.out.println("[lwjgl] GL_VERSION = " + glGetString(GL_VERSION));
        System.out.println("[lwjgl] GL_RENDERER = " + glGetString(GL_RENDERER));
        for (int i = 0; i < 10 && !glfwWindowShouldClose(win); i++) {
            glClearColor(0.3f, 0.6f, 0.9f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glfwSwapBuffers(win);
            glfwPollEvents();
        }
        System.out.println("[lwjgl] LWJGL WINDOW OK");
        glfwTerminate();
    }
}
