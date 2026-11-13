package dev.henge.probe;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

/**
 * The permissions this app asks for, and the discipline about when.
 *
 * Only one is asked for at startup: permission to post notifications. It is not decoration — a
 * download of a restore image is an hour-long operation that has to keep running in the background,
 * and a foreground service without a visible notification is not allowed to exist. Asking later
 * would mean asking after the download had already been killed.
 *
 * Everything else is asked for at the moment it is used, or not at all: the app reads and writes
 * only its own directories, and a picked IPSW arrives with a grant attached to it. There is no
 * "storage permission" here to grant, and no reason to ask for one.
 */
final class Permissions {

    static final int REQUEST_STARTUP = 100;
    static final int REQUEST_NOTIFICATIONS = 101;

    private Permissions() {
    }

    /** True when the one startup permission is already held (or not needed on this version). */
    static boolean startupGranted(Activity a) {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU
                || a.checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Asks for the startup permission if it is missing.
     *
     * Called once from the main screen. A refusal is not an error: the app works, the download just
     * cannot survive the app being closed, and the install screen says so when it matters.
     */
    static void askAtStartup(Activity a) {
        if (startupGranted(a)) return;
        a.requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, REQUEST_STARTUP);
    }

    /** Asks again, from the install screen, when a download is about to be started without it. */
    static void askForDownload(Activity a) {
        if (startupGranted(a)) return;
        a.requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"},
                REQUEST_NOTIFICATIONS);
    }

    /** Opens this app's page in the system settings, for a permission the user refused. */
    static void openSettings(Activity a) {
        Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", a.getPackageName(), null));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        a.startActivity(intent);
    }

    /** How the permission state reads on the settings screen. */
    static String summary(Activity a) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return "系统版本无需授权";
        return startupGranted(a) ? "已授权" : "未授权（下载无法在后台继续）";
    }
}
