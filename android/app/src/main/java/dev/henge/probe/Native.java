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

    /** Whether the GPU has taken the screen over (a mode is set and a frame was presented). */
    static native boolean nativeDisplayReady();

    /** Whether a machine is running in this process. */
    static native boolean nativeIsUp();

    /** Hands the display surface to the running machine's GPU device; call on every surface change. */
    static native void nativeAttachWindow(android.view.Surface surface);

    static native String nativeBoot(String rom, String disk, String aux, int ramMb, int ncpus,
                                    String cacheDir);

    /** Stops the guest and joins its threads. Safe when nothing is running. */
    static native void nativeStop();

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
