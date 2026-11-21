package dev.henge.probe;

/**
 * The engine's native side, as the app reaches it.
 *
 * Its own class so that more than one screen can call in: loading the library once per process is
 * the point of a static initialiser, and a second `System.loadLibrary` elsewhere would be a second
 * place to get wrong.
 */
final class Native {

    static {
        // libhenge.so carries the engine, the probe, and the Rust graphics stack statically
        // linked into it (android/CMakeLists.txt).
        System.loadLibrary("henge");
    }

    private Native() {
    }

    /**
     * Runs the whole diagnostic sweep and returns its report.
     *
     * @param driverDir native library directory of the driver to use, or null for the system loader
     * @param driverLib library name inside it, or null
     * @param tmpDir    a writable directory for the loader's renamed copy (unused on API 29+)
     */
    static native String nativeProbe(int bytes, String driverDir, String driverLib, String tmpDir);

    /**
     * Boots a guest from three files and leaves it running.
     *
     * Returns a report whatever happens: a missing file is the ordinary answer, and it comes back
     * as a sentence rather than as an exception or a crash.
     */
    /** The firmware's boot framebuffer as ARGB ints; false when there is not one yet. */
    static native boolean nativeBootFramebuffer(int[] pixels, int[] meta);

    /** Puts an app log line into the engine's stream, and so into its log file. */
    static native void nativeLog(String line);

    /** Each CPU's {pc, el, halted} flattened; returns how many CPUs were written. */
    static native int nativeCpuSnapshot(long[] out);

    /** Render state: {has mode, guest presents, host presents}. */
    static native void nativeRenderStats(int[] out);

    /** Whether the GPU has taken the screen over (a mode is set and a frame was presented). */
    static native boolean nativeDisplayReady();

    /** Whether a machine is running in this process. */
    static native boolean nativeIsUp();

    /** Hands the display surface to the running machine's GPU device; call on every surface change. */
    static native void nativeAttachWindow(android.view.Surface surface);

    static native String nativeBoot(String rom, String disk, String aux, int ramMb, int ncpus,
                                    String cacheDir);

    /**
     * Takes the display surface back from the running machine: its frame presents stop, and its
     * window reference is released. Call it when the surface goes away (the activity is put
     * away), and {@link #nativeAttachWindow} again when a new one exists.
     */
    static native void nativeDetachWindow();

    /** Stops the guest and joins its threads. Safe when nothing is running. */
    static native void nativeStop();

    /** One HID usage-page-7 key, as a key table produces it (0xe0..0xe7 are the modifiers). */
    static native void nativeKey(int usage, boolean down);

    /** The same by Linux input key code, for a key a hardware keyboard reported. */
    static native void nativeKeyLinux(int code, boolean down);

    /**
     * One finger on the guest's Magic Trackpad 2.
     *
     * @param id    a stable 1..15 for this finger
     * @param phase 0 down, 1 move, 2 up
     * @param x     the pad's sensor units, 0..8134
     * @param y     the pad's sensor units, 0..5206
     * @param tsMs  the touch sample's own clock
     */
    static native void nativeTouch(int id, int phase, int x, int y, long tsMs);

    /** Ends a batch of finger moves: one report for everything that moved since the last. */
    static native void nativeTouchFrame(long tsMs);

    /**
     * The absolute tablet: x and y in its own 0..32767 range, buttons 1..5, wheel in notches.
     *
     * A touch lands exactly where it is — no acceleration, no integration — which is what a touch
     * screen means, and what the 触控 mode needed all along.
     */
    static native void nativeTablet(int x16, int y16, int buttons, int wheel);

    /** A real mouse: relative movement in counted pixels the guest applies its own acceleration to. */
    static native void nativeMouseMove(int dx, int dy);

    /** Its buttons: 1 left, 2 right, 3 middle, as HID numbers. */
    static native void nativeMouseButton(int button, boolean down);

    /** Its wheel: whole notches, positive away from the user. */
    static native void nativeMouseWheel(int notches);

    /** The pad's click: down then up without movement is a click, held it is a drag. */
    static native void nativeTouchButton(boolean down);

    /** Everything the engine prints goes to this file; logcat only gets a rate-limited sample. */
    static native void nativeSetLogFile(String path);

    /** Creates the root disk and the auxiliary storage if they are not there yet. */
    /**
     * Unpacks the boot ROM from one compressed block of Apple's restore image.
     *
     * Returns null on success, or a sentence saying what was wrong with the block.
     */
    static native String nativeBootRomUnpack(byte[] block, String outPath);

    static native String nativeInstallPrepare(String dir, String diskPath, String auxPath,
                                              long diskBytes);

    /**
     * Boots the ROM in DFU mode and starts the restore. Returns at once; progress comes from
     * {@link #nativeInstallStatus}.
     */
    static native String nativeInstallStart(String rom, String disk, String aux, String ipsw,
                                            String workdir, int ramMb, int ncpus, long ecid,
                                            String cacheDir);

    /** idevicerestore's step, its percentage, and whether it is running, done or failed. */
    static native String nativeInstallStatus();

    /** Ends the running install; the next attempt builds a fresh machine. */
    static native void nativeInstallStop();
}
