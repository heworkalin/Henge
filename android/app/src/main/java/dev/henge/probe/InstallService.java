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

    static void start(Context c) {
        if (up) return;
        Intent intent = new Intent(c, InstallService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) c.startForegroundService(intent);
        else c.startService(intent);
    }

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
            stopSelf();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        up = false;
        AppLog.i("安装服务: onDestroy");
        super.onDestroy();
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
