package dev.henge.probe;

import android.content.Context;

import java.io.File;
import android.content.SharedPreferences;

/**
 * What the screens remember.
 *
 * One place, so the console that drives the engine and the settings screen that describes it
 * cannot drift apart. Values are stored by name, not index, so a reordered list does not silently
 * change which driver is selected.
 */
final class Prefs {

    private static final String FILE = "henge";

    static final String DRIVER_LABEL = "driver.label";
    static final String RAM_MB = "vm.ram_mb";
    static final String CPUS = "vm.cpus";
    static final String INSTANCE_NAME = "vm.name";

    static final int DEFAULT_RAM_MB = 2048;
    static final int DEFAULT_CPUS = 4;

    /**
     * Bumped by every write.
     *
     * A screen rebuilt on every resume is a screen that stutters on every resume; a screen that
     * rebuilds only when something it shows actually changed costs nothing. This counter is how it
     * knows, without asking each value in turn.
     */
    private static volatile long revision = 0;

    static long revision() {
        return revision;
    }

    private Prefs() {
    }

    private static android.content.SharedPreferences.Editor edit(Context c) {
        revision++;
        return sp(c).edit();
    }

    private static SharedPreferences sp(Context c) {
        return c.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    static String driverLabel(Context c) {
        return sp(c).getString(DRIVER_LABEL, null);
    }

    static void setDriverLabel(Context c, String label) {
        edit(c).putString(DRIVER_LABEL, label).apply();
    }

    static int ramMb(Context c) {
        return sp(c).getInt(RAM_MB, DEFAULT_RAM_MB);
    }

    static void setRamMb(Context c, int mb) {
        edit(c).putInt(RAM_MB, mb).apply();
    }

    static int cpus(Context c) {
        return sp(c).getInt(CPUS, DEFAULT_CPUS);
    }

    static void setCpus(Context c, int n) {
        edit(c).putInt(CPUS, n).apply();
    }

    static String releaseBuild(Context c) {
        return sp(c).getString("install.build", Releases.RECOMMENDED.build);
    }

    static void setReleaseBuild(Context c, String build) {
        edit(c).putString("install.build", build).apply();
    }

    /** "system", "light" or "dark"; see {@code Theme.dark}. */
    static String appearance(Context c) {
        return sp(c).getString("ui.appearance", "system");
    }

    static void setAppearance(Context c, String mode) {
        edit(c).putString("ui.appearance", mode).apply();
    }

    /** One of {@code Theme.ACCENTS}. */
    static String accent(Context c) {
        return sp(c).getString("ui.accent", "蓝");
    }

    static void setAccent(Context c, String name) {
        edit(c).putString("ui.accent", name).apply();
    }

    /** Whether the display's controls fade away while a guest is running. */
    static boolean hideControls(Context c) {
        return sp(c).getBoolean("display.hideControls", true);
    }

    static void setHideControls(Context c, boolean hide) {
        edit(c).putBoolean("display.hideControls", hide).apply();
    }

    /**
     * Whether this exact file has already passed the integrity check.
     *
     * Keyed by path, length and modification time: a 16 GB read per install would make every
     * attempt slower than the install itself, and any change to the file changes one of those. A
     * file replaced by an identical copy (same size, same timestamp) would be skipped — that is the
     * one case this cannot see, and it is not a case that happens by accident.
     */
    static boolean isVerified(Context c, File f) {
        android.content.SharedPreferences p = sp(c);
        return f != null && f.exists()
                && f.getAbsolutePath().equals(p.getString("ipsw.verified.path", null))
                && f.length() == p.getLong("ipsw.verified.size", -1)
                && f.lastModified() == p.getLong("ipsw.verified.mtime", -2);
    }

    static void markVerified(Context c, File f) {
        if (f == null || !f.exists()) return;
        sp(c).edit()
                .putString("ipsw.verified.path", f.getAbsolutePath())
                .putLong("ipsw.verified.size", f.length())
                .putLong("ipsw.verified.mtime", f.lastModified())
                .apply();
    }

    static void clearVerified(Context c) {
        sp(c).edit()
                .remove("ipsw.verified.path")
                .remove("ipsw.verified.size")
                .remove("ipsw.verified.mtime")
                .apply();
    }

    /** An IPSW the user brought in themselves; used instead of downloading when set. */
    static String localIpsw(Context c) {
        return sp(c).getString("install.localIpsw", null);
    }

    static void setLocalIpsw(Context c, String path) {
        if (path == null) edit(c).remove("install.localIpsw").apply();
        else edit(c).putString("install.localIpsw", path).apply();
    }

    static String instanceName(Context c) {
        return sp(c).getString(INSTANCE_NAME, "macOS");
    }

    static void setInstanceName(Context c, String name) {
        edit(c).putString(INSTANCE_NAME, name).apply();
    }
}
