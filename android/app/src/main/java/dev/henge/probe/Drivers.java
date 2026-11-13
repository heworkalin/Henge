package dev.henge.probe;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * The Vulkan drivers this app can open.
 *
 * One app is shipped, and the driver is inside it (`jniLibs/libvulkan_freedreno.so`, fetched by
 * `android/fetch-turnip.sh`). The system loader stays selectable, and so does any package that
 * declares the FCL-Team/FCLDriverPlugin contract — not because users are expected to install one,
 * but because it is the packaging contract this code speaks and it makes swapping a driver during
 * bring-up a matter of installing an APK.
 *
 * The name is deliberately not `libvulkan.so`: that is what the platform loader resolves, and such
 * a library in an APK would hijack Vulkan for the whole process.
 */
final class Drivers {

    /** One selectable driver: the system loader, the embedded library, or a plugin's. */
    static final class Driver {
        final String label;
        final String dir;
        final String lib;

        Driver(String label, String dir, String lib) {
            this.label = label;
            this.dir = dir;
            this.lib = lib;
        }

        boolean isSystem() {
            return dir == null || lib == null;
        }

        @Override
        public String toString() {
            return label;
        }
    }

    static final String EMBEDDED_LIB = "libvulkan_freedreno.so";

    private Drivers() {
    }

    /** Every driver, in the order the selection should offer them: embedded, plugins, system. */
    /**
     * Every driver this app could open.
     *
     * `getInstalledPackages` with metadata is a binder call that walks every package on the device,
     * and it used to run on the UI thread each time a screen showing a driver was built — which is
     * every tab switch. It is now cached, and a caller that needs it eagerly should warm it with
     * `warm` on a worker thread.
     */
    static List<Driver> all(Context context) {
        synchronized (CACHE) {
            if (CACHE.list != null) return CACHE.list;
        }
        List<Driver> drivers = new ArrayList<>();
        Driver embedded = embedded(context);
        if (embedded != null) drivers.add(embedded);
        drivers.addAll(plugins(context));
        drivers.add(new Driver("系统驱动 (libvulkan.so)", null, null));
        synchronized (CACHE) {
            CACHE.list = drivers;
        }
        return drivers;
    }

    /** Drops the cached list; called when a plugin may have appeared. */
    static void invalidate() {
        synchronized (CACHE) {
            CACHE.list = null;
        }
    }

    /** Fills the cache on a worker thread, so a later screen does not pay for it. */
    static void warm(Context context) {
        new Thread(() -> {
            try {
                all(context);
            } catch (Throwable ignored) {
            }
        }, "henge-drivers").start();
    }

    /** The driver bundled in this APK, if one was fetched. */
    static Driver embedded(Context context) {
        File so = new File(context.getApplicationInfo().nativeLibraryDir, EMBEDDED_LIB);
        if (!so.exists()) {
            AppLog.i("内置驱动: 未打包 (" + so + ")");
            return null;
        }
        AppLog.i("内置驱动: " + so + " (" + so.length() + " 字节)");
        return new Driver("内置 Turnip", context.getApplicationInfo().nativeLibraryDir, EMBEDDED_LIB);
    }

    /**
     * Installed packages that declare the FCL driver-plugin contract, read exactly as
     * FCLDriverPlugin writes it. A plugin whose library is missing is still listed, with that said
     * in its label: during bring-up, a visible failure beats a hidden entry.
     */
    static List<Driver> plugins(Context context) {
        List<Driver> found = new ArrayList<>();
        PackageManager pm = context.getPackageManager();
        List<PackageInfo> installed;
        try {
            installed = pm.getInstalledPackages(PackageManager.GET_META_DATA);
        } catch (Throwable t) {
            AppLog.w("枚举插件失败", t);
            return found;
        }
        for (PackageInfo pi : installed) {
            ApplicationInfo ai = pi.applicationInfo;
            if (ai == null || ai.metaData == null) continue;
            Bundle meta = ai.metaData;
            if (!meta.getBoolean("fclPlugin", false)) continue;

            String name = meta.getString("driver");
            String lib = meta.getString("driver_lib");
            String gpu = meta.getString("driver_gpu");
            if (lib == null || lib.isEmpty()) continue;

            String dir = ai.nativeLibraryDir;
            File present = dir == null ? null : new File(dir, lib);
            String suffix = (present == null || !present.exists())
                    ? "  [库缺失]"
                    : "  [" + (gpu == null ? "?" : gpu) + "]";
            found.add(new Driver((name == null ? pi.packageName : name) + suffix, dir, lib));
            AppLog.i("驱动插件: " + pi.packageName + " name=" + name + " lib=" + lib
                    + " gpu=" + gpu + " dir=" + dir);
        }
        if (found.isEmpty()) AppLog.i("驱动插件: 未安装");
        return found;
    }

    private static final class Cache {
        List<Driver> list;
    }

    private static final Cache CACHE = new Cache();

    /** The label the settings screen shows for a stored choice, falling back to the embedded one. */
    static Driver resolve(List<Driver> drivers, String storedLabel) {
        if (storedLabel != null) {
            for (Driver d : drivers) if (d.label.equals(storedLabel)) return d;
        }
        return drivers.isEmpty() ? null : drivers.get(0);
    }
}
