package dev.henge.probe;

/**
 * The machine's state, as the process sees it.
 *
 * The guest belongs to the process, not to a screen: the display activity is one window onto it,
 * and closing a window is not the same as powering the machine off. Before this existed, leaving
 * the display screen joined every CPU thread (DisplayActivity.onDestroy called nativeStop), so the
 * process had nothing left to keep it alive and Android reclaimed it -- a back press looked like a
 * crash.
 */
final class VmHost {
    private static boolean running;

    private VmHost() {}

    static boolean isRunning() {
        return running;
    }

    static void setRunning(boolean value) {
        running = value;
    }

    /** True when the engine has a machine that can be shown again without booting it anew. */
    static boolean canReopen() {
        return running && Native.nativeIsUp();
    }
}
