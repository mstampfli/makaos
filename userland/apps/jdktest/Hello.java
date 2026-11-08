public class Hello {
    public static void main(String[] args) {
        Runtime rt = Runtime.getRuntime();
        long mb = 1024 * 1024;
        System.out.println("Hello from OpenJDK Zero on MakaOS!");
        System.out.println("maxMemory  = " + (rt.maxMemory()  / mb) + " MB");
        System.out.println("totalMemory= " + (rt.totalMemory()/ mb) + " MB");
        // Allocate ~800 MB in 8 chunks of 100 MB to prove the heap is real RAM.
        int chunkMB = 100, chunks = 8;
        byte[][] blocks = new byte[chunks][];
        for (int i = 0; i < chunks; i++) {
            blocks[i] = new byte[chunkMB * (int) mb];
            for (int j = 0; j < blocks[i].length; j += 4096) blocks[i][j] = (byte) i;  // touch every page
            System.out.println("allocated + touched " + ((i + 1) * chunkMB) + " MB");
        }
        long used = (rt.totalMemory() - rt.freeMemory()) / mb;
        System.out.println("used heap  = " + used + " MB  (allocation of " + (chunkMB*chunks) + " MB succeeded)");
        System.out.println("2 + 2 = " + (2 + 2));
    }
}
