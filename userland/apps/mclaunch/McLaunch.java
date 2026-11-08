package makaos.launch;
// McLaunch - diagnostic entry point for launching Minecraft on MakaOS. Lives in a
// NAMED package: as the app's main class it must not pollute the default (unnamed)
// package, or its unsigned CodeSource conflicts with Minecraft's SIGNED
// default-package classes (fiq, a, b...) -> SecurityException "signer information
// does not match". The stock
// `java` launcher masks a main-class load failure behind its own localized error
// path (sun.launcher.resources.launcher), which itself fails to load on MakaOS,
// so the real cause never prints. This wrapper prints its own environment + a few
// self-tests, then loads net.minecraft.client.main.Main reflectively and prints
// the FULL throwable + cause chain to stderr itself.
import java.security.MessageDigest;
import java.util.jar.JarFile;
import java.util.jar.JarEntry;
import java.io.InputStream;

public class McLaunch {
    static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x", x));
        return sb.toString();
    }
    public static void main(String[] args) throws Throwable {
        System.err.println("[mclaunch] alive; os.name=" + System.getProperty("os.name")
                + " locale=" + java.util.Locale.getDefault());
        // Heartbeat: a lightweight liveness tick to RAW fd 2 (bypassing any
        // System.err log4j reassigns).  Deliberately does NOT call
        // Thread.getAllStackTraces() -- a full VM_ThreadDump walks every thread's
        // frames at a safepoint and SIGSEGVs in frame::interpreter_frame_method
        // on this JVM/MakaOS build.  Just prints uptime + heap so a hang is
        // visible without triggering that crash.
        Thread hb = new Thread(() -> {
            java.io.OutputStream raw = new java.io.FileOutputStream(java.io.FileDescriptor.err);
            Runtime rt = Runtime.getRuntime();
            try {
                for (long t = 0; ; t += 20) {
                    Thread.sleep(20000);
                    long used = (rt.totalMemory() - rt.freeMemory()) >> 20;
                    StringBuilder sb = new StringBuilder("[hb] +" + (t+20) + "s heap=" + used + "M | ");
                    // Enumerate thread NAMES + getState() only (getState reads a
                    // field -- NO safepoint/VM_ThreadDump, so it does not trigger
                    // the frame-walk SIGSEGV that getAllStackTraces does).
                    ThreadGroup g = Thread.currentThread().getThreadGroup();
                    while (g.getParent() != null) g = g.getParent();
                    Thread[] arr = new Thread[g.activeCount() + 16];
                    int n = g.enumerate(arr, true);
                    for (int i = 0; i < n; i++) {
                        Thread th = arr[i];
                        if (th == null) continue;
                        String nm = th.getName();
                        if (nm.equals("Render thread") || nm.equals("main")
                                || nm.startsWith("process reaper") || nm.contains("Worker"))
                            sb.append(nm).append('=').append(th.getState()).append("  ");
                    }
                    raw.write((sb.toString() + "\n").getBytes("UTF-8"));
                    raw.flush();
                }
            } catch (Throwable t) { /* daemon: ignore */ }
        }, "makaos-heartbeat");
        hb.setDaemon(true);
        hb.start();
        // Probe ManagementFactory.getRuntimeMXBean() FIRST so we catch the
        // original PlatformMBeanProviderImpl constructor exception with its real
        // cause (later callers only see a cached NoClassDefFoundError).  MC's
        // Main.main -> ac.k needs this; if it throws, MC exits at startup.
        try {
            java.lang.management.ManagementFactory.getRuntimeMXBean();
            System.err.println("[mclaunch] RuntimeMXBean OK");
        } catch (Throwable t) {
            System.err.println("[mclaunch] RuntimeMXBean FAILED, full chain:");
            for (Throwable c = t; c != null; c = c.getCause()) {
                System.err.println("[mclaunch]   >> " + c);
                StackTraceElement[] st = c.getStackTrace();
                for (int k = 0; k < Math.min(st.length, 6); k++)
                    System.err.println("[mclaunch]        at " + st[k]);
            }
        }
        // Crypto self-test: SHA-256("abc") must be ba7816bf...20015ad. A wrong
        // value means the JVM's MessageDigest is broken, which breaks jar verify.
        try {
            String h = hex(MessageDigest.getInstance("SHA-256").digest("abc".getBytes("UTF-8")));
            System.err.println("[mclaunch] SHA-256(abc)=" + h);
            System.err.println("[mclaunch]   expected ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        } catch (Throwable t) { System.err.println("[mclaunch] SHA-256 test threw: " + t); }
        // Jar signer inspection: read a few classes from the SIGNED client.jar and
        // report their code signers. Inconsistent results = the verify path fails.
        try (JarFile jf = new JarFile("/mc/client.jar", true)) {
            for (String n : new String[]{"fiq.class", "a.class", "net/minecraft/client/main/Main.class"}) {
                JarEntry e = jf.getJarEntry(n);
                if (e == null) { System.err.println("[mclaunch] jar: " + n + " ABSENT"); continue; }
                try (InputStream is = jf.getInputStream(e)) {
                    byte[] buf = new byte[16384]; int tot = 0, r;
                    while ((r = is.read(buf)) != -1) tot += r;
                    Object sg = e.getCodeSigners();
                    System.err.println("[mclaunch] jar: " + n + " bytes=" + tot
                            + " signers=" + (sg == null ? "null" : ((java.security.CodeSigner[]) sg).length));
                } catch (Throwable t) {
                    System.err.println("[mclaunch] jar: " + n + " read/verify threw: " + t);
                }
            }
        } catch (Throwable t) { System.err.println("[mclaunch] jar open threw: " + t); }

        // Direct load test of the JNA native (jna.nounpack hides the real error).
        try { System.load("/mc/natives/libjnidispatch.so");
              System.err.println("[mclaunch] libjnidispatch loaded OK"); }
        catch (Throwable t) { System.err.println("[mclaunch] libjnidispatch load FAILED: " + t); }


        try {
            Class<?> c = Class.forName("net.minecraft.client.main.Main");
            System.err.println("[mclaunch] Main class loaded, invoking main(" + args.length + " args)");
            java.lang.reflect.Method m = c.getMethod("main", String[].class);
            m.invoke(null, (Object) args);
        } catch (Throwable t) {
            System.err.println("[mclaunch] FAILED, cause chain:");
            for (Throwable cur = t; cur != null; cur = cur.getCause())
                System.err.println("[mclaunch]   " + cur.getClass().getName() + ": " + cur.getMessage());
            t.printStackTrace();
        }
    }
}
