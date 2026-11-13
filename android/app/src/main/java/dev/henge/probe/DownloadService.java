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

import java.io.File;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Downloads a restore image in a foreground service, so leaving the app does not end it.
 *
 * Sixteen gigabytes over a phone's connection is an hour-scale operation, and an hour is long
 * enough that the user will do something else. A thread owned by an activity dies with the
 * activity; a download owned by a foreground service does not, and the notification is how a
 * service says it is still working — which is also the only way the platform lets an app keep
 * working in the background at all.
 *
 * The state is a single small object plus listeners, not a broadcast: the installer screen and the
 * notification both read the same numbers, and neither can drift from the other.
 */
public class DownloadService extends Service {

    private static final String CHANNEL = "download";
    private static final int NOTIFY_ID = 0x48454E47;   // 'HENG'

    static final String ACTION_START = "dev.henge.probe.DOWNLOAD_START";
    static final String ACTION_CANCEL = "dev.henge.probe.DOWNLOAD_CANCEL";
    private static final String EXTRA_URL = "url";
    private static final String EXTRA_NAME = "name";
    private static final String EXTRA_BYTES = "bytes";

    /** What the download is doing, readable from any thread and from anywhere in the app. */
    static final class State {
        static final String IDLE = "idle";
        static final String RUNNING = "running";
        static final String DONE = "done";
        static final String FAILED = "failed";
        static final String CANCELLED = "cancelled";

        volatile String phase = IDLE;
        volatile String name = "";
        volatile long received;
        volatile long total;
        volatile String message = "";
        /** Bytes per second, smoothed; 0 until two reports have been measured. */
        volatile long speed;
        /** Milliseconds left, or 0 when it cannot be estimated yet. */
        volatile long etaMs;
        /** The finished file, once there is one. */
        volatile File file;
    }

    private static final State STATE = new State();
    /**
     * How often progress may reach the UI and the notification.
     *
     * The downloader reports every 128 KiB per connection; four connections at speed is thousands
     * of callbacks a second, and every one of them used to post to the main thread and call the
     * notification manager over binder. The bytes still arrive at full speed — only the reporting
     * is slowed to a rate a person can see and a phone can afford.
     */
    private static final long REPORT_INTERVAL_MS = 250;
    private static final CopyOnWriteArrayList<Runnable> LISTENERS = new CopyOnWriteArrayList<>();
    private static volatile Thread worker;

    /** The current state; the same object every time, so a reader sees it move. */
    static State state() {
        return STATE;
    }

    /** Registers a listener for progress; called on the download's thread, so post to a view. */
    static void listen(Runnable r) {
        LISTENERS.add(r);
    }

    static void unlisten(Runnable r) {
        LISTENERS.remove(r);
    }

    private static void notifyListeners() {
        for (Runnable r : LISTENERS) {
            try {
                r.run();
            } catch (Throwable ignored) {
            }
        }
    }

    static boolean running() {
        return STATE.phase.equals(State.RUNNING);
    }

    /** Asks the service to download `release` into the app's own directory. */
    static void start(Context c, Releases.Release release) {
        Intent intent = new Intent(c, DownloadService.class);
        intent.setAction(ACTION_START);
        intent.putExtra(EXTRA_URL, release.url);
        intent.putExtra(EXTRA_NAME, release.build);
        intent.putExtra(EXTRA_BYTES, release.bytes);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) c.startForegroundService(intent);
        else c.startService(intent);
    }

    /** Asks the service to stop, keeping what it has for the next attempt. */
    static void cancel(Context c) {
        Intent intent = new Intent(c, DownloadService.class);
        intent.setAction(ACTION_CANCEL);
        c.startService(intent);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        createChannel(this);

        if (intent != null && ACTION_CANCEL.equals(intent.getAction())) {
            if (worker != null) worker.interrupt();
            return START_NOT_STICKY;
        }

        if (intent == null || intent.getAction() == null) {
            // Restarted by the platform without its extras: nothing to resume from here, since the
            // figure it needs lives in the tab that asked for it.
            stopSelf();
            return START_NOT_STICKY;
        }

        String url = intent.getStringExtra(EXTRA_URL);
        String name = intent.getStringExtra(EXTRA_NAME);
        long bytes = intent.getLongExtra(EXTRA_BYTES, 0);
        if (url == null || name == null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (running()) {
            AppLog.i("下载服务: 已有下载在进行，忽略新请求");
            return START_NOT_STICKY;
        }

        startForeground(NOTIFY_ID, buildNotification("准备下载…", 0, 0, true));
        startWorker(url, name, bytes);
        return START_NOT_STICKY;
    }

    private void startWorker(String url, String name, long bytes) {
        final File vm = new File(getExternalFilesDir(null), "vm");
        final AtomicBoolean cancelled = new AtomicBoolean(false);

        STATE.phase = State.RUNNING;
        STATE.name = name;
        STATE.received = 0;
        STATE.total = bytes;
        STATE.speed = 0;
        STATE.etaMs = 0;
        STATE.message = "";
        STATE.file = new File(vm, name + ".ipsw");
        notifyListeners();

        worker = new Thread(() -> {
            File part = new File(STATE.file.getPath() + ".part");
            final long[] lastReport = {0L};
            // Speed is measured between reports and smoothed, because a single 250ms sample of a
            // mobile connection swings by a factor of ten and an estimate that jumps around is
            // worse than none.
            final long[] lastBytes = {0L};
            final long[] lastAt = {0L};
            String problem = Downloader.run(url, part, STATE.file, bytes,
                    (received, total) -> {
                        // The counters are always current; the reporting is rationed.
                        STATE.received = received;
                        if (total > 0) STATE.total = total;
                        long now = android.os.SystemClock.elapsedRealtime();
                        if (now - lastReport[0] < REPORT_INTERVAL_MS) return;
                        if (lastAt[0] > 0) {
                            long deltaBytes = received - lastBytes[0];
                            long deltaMs = now - lastAt[0];
                            if (deltaMs > 0 && deltaBytes >= 0) {
                                long instant = deltaBytes * 1000L / deltaMs;
                                STATE.speed = STATE.speed == 0 ? instant
                                        : (STATE.speed * 3 + instant) / 4;
                            }
                        }
                        lastBytes[0] = received;
                        lastAt[0] = now;
                        lastReport[0] = now;
                        STATE.etaMs = (STATE.speed > 0 && STATE.total > received)
                                ? (STATE.total - received) * 1000L / STATE.speed : 0;
                        int percent = STATE.total > 0
                                ? (int) (received * 100 / STATE.total) : 0;
                        updateNotification("正在下载 " + name, percent, STATE.total, false);
                        notifyListeners();
                    },
                    cancelled);
            if (cancelled.get() || Thread.currentThread().isInterrupted()) {
                STATE.phase = State.CANCELLED;
                STATE.message = "已停止，可续传";
            } else if (problem != null) {
                STATE.phase = State.FAILED;
                STATE.message = problem;
                AppLog.w("下载服务: " + problem, null);
            } else {
                STATE.phase = State.DONE;
                STATE.received = STATE.file.length();
                STATE.total = STATE.received;
                STATE.message = "";
                AppLog.i("下载服务: 完成 " + STATE.file.getName());
            }
            notifyListeners();
            stopForeground(false);
            if (STATE.phase == State.DONE) {
                updateNotification("下载完成 " + name, 100, 100, false);
            } else {
                updateNotification("下载停止 " + name + "（" + STATE.message + "）", 0, 0, false);
            }
            // Left on screen so the outcome is visible after the app has been closed; the platform
            // lets it be dismissed by hand.
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) nm.cancel(NOTIFY_ID);
            stopSelf();
        }, "henge-download");
        worker.start();
    }

    private void updateNotification(String text, int percent, long total, boolean indeterminate) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.notify(NOTIFY_ID, buildNotification(text, percent, total, indeterminate));
    }

    private Notification buildNotification(String text, int percent, long total,
                                          boolean indeterminate) {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent content = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Intent stop = new Intent(this, DownloadService.class).setAction(ACTION_CANCEL);
        PendingIntent stopIntent = PendingIntent.getService(this, 1, stop,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        String line = total > 0 && !indeterminate
                ? text + " · " + AppLog.bytes(STATE.received) + " / " + AppLog.bytes(total)
                : text;

        Notification.Builder b = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        b.setContentTitle("Henge")
                .setContentText(line)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentIntent(content)
                .setOngoing(indeterminate || percent < 100)
                .setOnlyAlertOnce(true)
                .addAction(new Notification.Action.Builder(null, "停止", stopIntent).build());
        if (indeterminate) {
            b.setProgress(0, 0, true);
        } else if (percent >= 100) {
            b.setProgress(0, 0, false);
            b.setSmallIcon(android.R.drawable.stat_sys_download_done);
        } else {
            b.setProgress(100, percent, false);
        }
        return b.build();
    }

    private static void createChannel(Context c) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm == null || nm.getNotificationChannel(CHANNEL) != null) return;
        NotificationChannel channel = new NotificationChannel(CHANNEL, "下载",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("macOS 安装映像的下载进度");
        channel.setShowBadge(false);
        nm.createNotificationChannel(channel);
    }

    @Override
    public void onDestroy() {
        if (worker != null && worker.isAlive()) worker.interrupt();
        super.onDestroy();
    }
}
