package dev.henge.probe;

import android.util.Log;

import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * The app's own log, for the log screen.
 *
 * Android does not let an app read other processes' logcat, so a log view has to be fed by the
 * app itself. This keeps a bounded ring of lines and mirrors each to logcat, which keeps the two
 * views (on screen, and over adb) in step.
 *
 * Deliberately not a general logging framework: one tag, one level, a ring, and listeners. The
 * engine's own output arrives through the same call sites that write here.
 */
public final class AppLog {

    public static final String TAG = "HENGE";
    private static final int CAPACITY = 2000;

    /** One line: when, at what level, and what. */
    public static final class Entry {
        public final long time;
        public final int level;      // one of android.util.Log's priorities
        public final String message;

        Entry(long time, int level, String message) {
            this.time = time;
            this.level = level;
            this.message = message;
        }

        public String format() {
            String stamp = new SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(new Date(time));
            return stamp + " " + levelName(level) + " " + message;
        }
    }

    /** Told when a line arrives; called on whichever thread wrote it. */
    public interface Listener {
        void onLine(Entry entry);
    }

    private static final ArrayDeque<Entry> LINES = new ArrayDeque<>();
    private static final List<Listener> LISTENERS = new ArrayList<>();

    private AppLog() {
    }

    public static void addListener(Listener l) {
        synchronized (LISTENERS) {
            if (!LISTENERS.contains(l)) LISTENERS.add(l);
        }
    }

    public static void removeListener(Listener l) {
        synchronized (LISTENERS) {
            LISTENERS.remove(l);
        }
    }

    public static void d(String message) { write(Log.DEBUG, message); }

    public static void i(String message) { write(Log.INFO, message); }

    public static void w(String message, Throwable t) {
        write(Log.WARN, t == null ? message : message + ": " + Log.getStackTraceString(t));
    }

    /** A line from somewhere else that already reached logcat: keep the two views identical. */
    public static void write(int level, String message) {
        Entry entry = new Entry(System.currentTimeMillis(), level, message);
        synchronized (LINES) {
            if (LINES.size() >= CAPACITY) LINES.removeFirst();
            LINES.addLast(entry);
        }
        // Into the engine's log file as well. logcat's main buffer is 256 KiB and the system's own
        // services fill it within seconds, so a line mirrored only there is gone before anyone can
        // read it; the file is the only record that survives.
        try {
            Native.nativeLog(message);
        } catch (Throwable ignored) {
            // Logging must never be a reason for the app to fail.
        }

        // Mirror to logcat so `adb logcat -s HENGE` shows the same thing the screen does.
        switch (level) {
            case Log.ERROR: Log.e(TAG, message); break;
            case Log.WARN: Log.w(TAG, message); break;
            case Log.INFO: Log.i(TAG, message); break;
            default: Log.d(TAG, message); break;
        }
        synchronized (LISTENERS) {
            for (Listener l : LISTENERS) l.onLine(entry);
        }
    }

    /** Every line kept, oldest first. */
    public static List<Entry> snapshot() {
        synchronized (LINES) {
            return new ArrayList<>(LINES);
        }
    }

    public static String dump() {
        StringBuilder sb = new StringBuilder();
        for (Entry e : snapshot()) sb.append(e.format()).append('\n');
        return sb.toString();
    }

    /** Bytes as a person reads them: MB and GB, one decimal where it helps. */
    public static String bytes(long n) {
        if (n <= 0) return "0 B";
        if (n >= (1L << 30)) return String.format(Locale.US, "%.1f GB", n / (double) (1L << 30));
        if (n >= (1L << 20)) return String.format(Locale.US, "%.0f MB", n / (double) (1L << 20));
        if (n >= (1L << 10)) return String.format(Locale.US, "%.0f KB", n / (double) (1L << 10));
        return n + " B";
    }

    /** Milliseconds as an estimate a person can act on ("还需 1 小时 3 分"). */
    public static String eta(long millis) {
        return duration(Math.max(0, millis / 1000));
    }

    /** Whole seconds as minutes and seconds, for an estimate that does not pretend to be precise. */
    public static String duration(long seconds) {
        if (seconds < 60) return seconds + " 秒";
        long minutes = seconds / 60;
        if (minutes < 60) return minutes + " 分钟";
        return (minutes / 60) + " 小时 " + (minutes % 60) + " 分";
    }

    public static void clear() {
        synchronized (LINES) {
            LINES.clear();
        }
    }

    private static String levelName(int level) {
        switch (level) {
            case Log.ERROR: return "E";
            case Log.WARN: return "W";
            case Log.INFO: return "I";
            case Log.VERBOSE: return "V";
            default: return "D";
        }
    }
}
