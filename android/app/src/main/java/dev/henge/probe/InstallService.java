package dev.henge.probe;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/**
 * Keeps an install alive while the screen is elsewhere.
 *
 * A restore runs for tens of minutes, and it is the app's own threads doing the work — the same
 * threads the platform reclaims when it decides a backgrounded process is worth more as memory.
 * A foreground notification is the only way an app says "still working, do not collect", and the
 * install is exactly the case where being collected loses an hour.
 *
 * The service owns no work of its own: it exists to hold the process up and to show what step the
 * installer is on. Stopping it early would take the machine's CPU threads with it.
 */
public class InstallService extends Service {

    private static final String CHANNEL = "install";
    private static final int NOTIFY_ID = 0x48454E48;   // 'HENH'

    /** Set once the service has actually gone foreground; guards against asking twice. */
    private static volatile boolean up;
    /**
     * Which run the service is up for. A finished thread from an earlier attempt must not stop
     * the service a later attempt has just started: it would take the new run's foreground away
     * and the platform would then end the process for starting a foreground service and never
     * entering the foreground, which is what a "tap again and it crashes" looked like.
     */
    private static final java.util.concurrent.atomic.AtomicInteger generation =
            new java.util.concurrent.atomic.AtomicInteger();

    /** Starts the service and returns the id of this run (see {@link #generation}). */
    static int start(Context c) {
        /* No "already up" short-circuit: the flag only says an instance exists, and after a failed
         * run the instance can be on its way out while the screen still believes it is running.
         * A second start command is harmless (onStartCommand re-enters the foreground), while a
         * skipped one leaves the caller doing a restore with no foreground service behind it. */
        int g = generation.incrementAndGet();
        Intent intent = new Intent(c, InstallService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) c.startForegroundService(intent);
        else c.startService(intent);
        return g;
    }

    /** Whether the run that was handed this id is still the current one. */
    static boolean current(int run) {
        return generation.get() == run;
    }

    /** Whether an install service is up: the screen's button state is derived from this rather
     *  than from a flag of its own, so a re-created screen still knows a restore is in progress. */
    static boolean running() {
        return up;
    }

    /** Stops the service, but only if its run is still this one. */
    static void stop(Context c, int run) {
        if (!current(run)) {
            AppLog.i("安装服务: 运行 " + run + " 已被更新的运行取代，不停止服务");
            return;
        }
        c.stopService(new Intent(c, InstallService.class));
    }

    /** Stops whatever run is current, for the screen's own "stop" button. */
    static void stop(Context c) {
        c.stopService(new Intent(c, InstallService.class));
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        AppLog.i("安装服务: onCreate");
        /*
         * Foreground here, not in onStartCommand: the platform gives a started service a few
         * seconds to say so, and the first call of a restore can hold the main thread long enough
         * for that to expire — which ends the process and the install with it.
         */
        try {
            createChannel(this);
            startForeground(NOTIFY_ID, note("正在安装 macOS…", 0));
            up = true;
            AppLog.i("安装服务: onCreate 已进入前台");
        } catch (Throwable t) {
            /* Swallowing this leaves the process on the platform's "started a foreground service
             * and never said so" timer, which ends the app a few seconds later. Say it failed and
             * go away instead of leaving that timer armed. */
            AppLog.w("安装服务: onCreate 进入前台失败", t);
            finishForeground();
            stopSelf();
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Logged on both sides of the call: the platform kills the process when this does not
        // happen within a few seconds, and the only question worth answering then is whether it
        // was reached at all, or threw.
        AppLog.i("安装服务: onStartCommand");
        try {
            createChannel(this);
            startForeground(NOTIFY_ID, note("正在安装 macOS…", 0));
            up = true;
            AppLog.i("安装服务: 已进入前台");
        } catch (Throwable t) {
            AppLog.w("安装服务: 进入前台失败", t);
            finishForeground();
            stopSelf();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        up = false;
        AppLog.i("安装服务: onDestroy");
        finishForeground();
        super.onDestroy();
    }

    /**
     * Leaves the foreground and takes the notification away. A finished or failed run must not
     * leave its "installing…" card behind: it says work is happening when nothing is, and the next
     * attempt would update a notification the user already believes belongs to a dead run.
     */
    private void finishForeground() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE);
            else stopForeground(true);
        } catch (Throwable t) {
            AppLog.w("安装服务: 退出前台失败", t);
        }
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.cancel(NOTIFY_ID);
    }

    /** Says what the installer is doing; called from the screen's polling thread. */
    static void update(Context c, String text, int percent) {
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm == null) return;
        createChannel(c);
        Notification.Builder b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(c, CHANNEL)
                : new Notification.Builder(c);
        Intent open = new Intent(c, MainActivity.class);
        PendingIntent content = PendingIntent.getActivity(c, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        b.setContentTitle("Henge")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentIntent(content)
                .setOngoing(true)
                .setOnlyAlertOnce(true);
        if (percent > 0) b.setProgress(100, percent, false);
        else b.setProgress(0, 0, true);
        nm.notify(NOTIFY_ID, b.build());
    }

    private Notification note(String text, int percent) {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent content = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        b.setContentTitle("Henge")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentIntent(content)
                .setOngoing(true);
        if (percent > 0) b.setProgress(100, percent, false);
        else b.setProgress(0, 0, true);
        return b.build();
    }

    private static void createChannel(Context c) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm == null || nm.getNotificationChannel(CHANNEL) != null) return;
        NotificationChannel channel = new NotificationChannel(CHANNEL, "安装",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("macOS 安装进度");
        channel.setShowBadge(false);
        nm.createNotificationChannel(channel);
    }
}
