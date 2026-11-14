package dev.henge.probe;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Downloads a restore image, over several connections, and resumes from what is already on disk.
 *
 * Two things decide whether a sixteen gigabyte download is bearable, and neither is the network:
 *
 *   - **Connections.** One TCP stream gets a fraction of what a CDN will hand out, because a single
 *     stream is limited by its own window and by how the route behaves. Apple's CDN answers range
 *     requests, so the file is split into fixed chunks and several workers pull them at once. The
 *     chunk a worker takes next is coordinated through an atomic counter — no locks in the loop,
 *     no thread owning "its" chunk after it has written it.
 *   - **Resumption.** The bytes already on disk are worth keeping: a dropped connection, a killed
 *     app, a user scrolling away. Each finished chunk is recorded in a small bookkeeping file, so a
 *     restart fetches only what is missing rather than re-fetching sixteen gigabytes.
 *
 * A server that ignores ranges (200 instead of 206) is not segmented at all: it is probed first,
 * and a server that will not do ranges gets a single stream that appends, which is the only
 * correct thing to do with a body that is always the whole file.
 */
final class Downloader {

    interface Progress {
        /** `total` is 0 until the size is known. */
        void onProgress(long received, long total);
    }

    /** 32 MiB: large enough that per-connection overhead vanishes, small enough to resume finely. */
    private static final long CHUNK = 32L << 20;
    /** Four is what a phone's radio and Apple's CDN both sustain; more adds heat, not speed. */
    private static final int WORKERS = 4;
    /** Returned by `segmented` when the server would not honour a range, so the caller can fall
     *  back to a single stream instead of reporting a failure. */
    private static final String RANGES_REFUSED = "\u0000ranges-refused";

    /**
     * The connections that are open right now.
     *
     * Cancelling cannot be done with Thread.interrupt alone: a thread blocked reading a socket
     * does not see the interrupt, and the read timeout is two minutes — long enough that "stop"
     * looks broken. Closing the connection is what actually ends the read.
     */
    private static final java.util.List<HttpURLConnection> OPEN =
            new java.util.concurrent.CopyOnWriteArrayList<>();

    /** Ends every download in this process: the reads fail, the workers see `cancelled`. */
    static void abort() {
        for (HttpURLConnection c : OPEN) {
            try {
                c.disconnect();
            } catch (Throwable ignored) {
            }
        }
        OPEN.clear();
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        OPEN.add(c);
        return c;
    }

    private static void shut(HttpURLConnection c) {
        OPEN.remove(c);
        try {
            c.disconnect();
        } catch (Throwable ignored) {
        }
    }

    private Downloader() {
    }

    /**
     * Downloads until `ipsw` holds the whole image.
     *
     * Returns null when complete, or a sentence saying why it stopped (cancellation included — the
     * caller decides whether that is a failure).
     */
    static String run(String url, File part, File ipsw, long expected, Progress progress,
                      AtomicBoolean cancelled) {
        File parent = part.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) return "无法创建目录 " + parent;

        File marks = new File(part.getPath() + ".chunks");
        long total = expected;
        try {
            long probed = probe(url);
            if (probed > 0) {
                total = probed;
                AppLog.i("下载: 服务器报告 " + AppLog.bytes(total)
                        + "，分 4 段并发下载" + (marks.exists() ? "（接着断点）" : ""));
            } else {
                // The probe is a courtesy, not a gate. We know the size from the listing, Apple
                // serves ranges, and the first chunk will say so if this particular server does
                // not — so the segmented path is the default even when the probe said nothing.
                AppLog.i("下载: 探测无应答，仍按 " + AppLog.bytes(total)
                        + " 分段下载，若服务器不支持 Range 再退回单连接");
            }
        } catch (Throwable t) {
            AppLog.i("下载: 探测异常（" + t + "），仍尝试分段");
        }

        String problem;
        if (total > 0) {
            problem = segmented(url, part, marks, total, progress, cancelled);
            if (RANGES_REFUSED.equals(problem)) {
                AppLog.i("下载: 服务器不支持 Range，改为单连接");
                problem = stream(url, part, expected, progress, cancelled);
            }
        } else {
            problem = stream(url, part, expected, progress, cancelled);
        }
        if (problem != null) return problem;

        if (!part.renameTo(ipsw)) {
            try {
                copy(part, ipsw);
            } catch (IOException e) {
                return "无法写出 " + ipsw + ": " + e;
            }
            part.delete();
        }
        marks.delete();
        AppLog.i("下载: 完成 " + ipsw.getName() + "（" + AppLog.bytes(ipsw.length()) + "）");
        return null;
    }

    /**
     * Asks for one byte to learn two things: whether ranges are honoured, and how large the file is.
     *
     * Returns the file's size when the answer was a 206 with a `Content-Range`, or 0 when the
     * server or the redirect chain will not play.
     */
    private static long probe(String url) throws IOException {
        HttpURLConnection c = open(url);
        try {
            Tls.apply(c);
            c.setInstanceFollowRedirects(true);
            c.setConnectTimeout(30_000);
            c.setReadTimeout(60_000);
            c.setRequestProperty("User-Agent", "Henge/Android");
            c.setRequestProperty("Range", "bytes=0-0");
            int code = c.getResponseCode();
            if (code == HttpURLConnection.HTTP_PARTIAL) {
                String range = c.getHeaderField("Content-Range");
                if (range != null) {
                    int slash = range.lastIndexOf('/');
                    if (slash > 0) {
                        try {
                            return Long.parseLong(range.substring(slash + 1).trim());
                        } catch (NumberFormatException ignored) {
                            // Fall through to a plain HEAD: the size alone still lets us segment.
                        }
                    }
                }
            }
            long length = c.getContentLengthLong();
            if (length > 0) {
                AppLog.i("下载: 探测只拿到长度 " + AppLog.bytes(length) + "（" + code + "）");
                return length;
            }
            return 0;
        } finally {
            shut(c);
        }
    }

    /**
     * Fetches the file chunk by chunk over several connections, skipping what is recorded as done.
     *
     * The bookkeeping is one byte per chunk in a file beside the download: 0 not held, 1 held. That
     * is crude and it is enough — a partially written chunk is simply fetched again, and a chunk
     * marked done was written and flushed before the mark.
     */
    private static String segmented(String url, File part, File marks, long total,
                                    Progress progress, AtomicBoolean cancelled) {
        int count = (int) ((total + CHUNK - 1) / CHUNK);
        byte[] done;
        try {
            done = readMarks(marks, count);
        } catch (IOException e) {
            return "无法读取断点记录: " + e;
        }

        try (RandomAccessFile file = new RandomAccessFile(part, "rw")) {
            if (file.length() != total) file.setLength(total);
        } catch (IOException e) {
            return "无法创建 " + part + ": " + e;
        }

        AtomicBoolean refused = new AtomicBoolean(false);
        AtomicInteger next = new AtomicInteger(0);
        AtomicLong held = new AtomicLong(0);
        for (int i = 0; i < count; i++) {
            if (done[i] == 1) held.addAndGet(chunkSize(i, total));
        }
        progress.onProgress(held.get(), total);

        Object marksLock = new Object();
        final String[] failure = {null};
        final Object failureLock = new Object();

        /* Set when a chunk has failed for good: every worker stops instead of walking on to the
           next chunk, which is what made a slow server look like a hang with no end. */
        final AtomicBoolean stopped = new AtomicBoolean(false);

        Runnable worker = () -> {
            while (!cancelled.get() && !refused.get() && !stopped.get()) {
                int index = next.getAndIncrement();
                if (index >= count) return;
                if (done[index] == 1) continue;
                long start = index * CHUNK;
                long end = Math.min(total, start + CHUNK) - 1;
                boolean got = false;
                for (int attempt = 0; attempt < 3 && !cancelled.get() && !stopped.get(); attempt++) {
                    try {
                        long wrote = fetchInto(url, part, start, end, held, total, progress, cancelled);
                        if (wrote != end - start + 1) throw new IOException("段 " + index + " 只写到 " + wrote);
                        synchronized (marksLock) {
                            done[index] = 1;
                            writeMarks(marks, done, index);
                        }
                        got = true;
                        break;
                    } catch (RangesRefused e) {
                        refused.set(true);
                        return;
                    } catch (Throwable t) {
                        if (cancelled.get()) return;
                        AppLog.i("下载: 段 " + index + " 第 " + (attempt + 1) + " 次失败: " + t);
                        synchronized (failureLock) {
                            failure[0] = t.toString();
                        }
                        try {
                            Thread.sleep(1000L << attempt);
                        } catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            return;
                        }
                    }
                }
                if (!got) {
                    stopped.set(true);
                    return;
                }
            }
        };

        Thread[] threads = new Thread[WORKERS];
        for (int i = 0; i < threads.length; i++) {
            threads[i] = new Thread(worker, "henge-dl-" + i);
            threads[i].start();
        }
        for (Thread t : threads) {
            try {
                t.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return "已取消";
            }
        }
        if (cancelled.get()) return "已取消";
        if (refused.get()) return RANGES_REFUSED;
        // A failed chunk leaves the file short: say so rather than hand the installer a truncated
        // image, which would fail much later and much less clearly.
        for (int i = 0; i < count; i++) {
            if (done[i] != 1) return failure[0] != null ? "下载不完整: " + failure[0] : "下载不完整";
        }
        return null;
    }

    /** One range request, written straight into the file at `start`. */
    private static long fetchInto(String url, File part, long start, long end, AtomicLong held,
                                  long total, Progress progress, AtomicBoolean cancelled)
            throws IOException {
        HttpURLConnection c = open(url);
        try {
            Tls.apply(c);
            c.setInstanceFollowRedirects(true);
            c.setConnectTimeout(30_000);
            c.setReadTimeout(120_000);
            c.setRequestProperty("User-Agent", "Henge/Android");
            c.setRequestProperty("Range", "bytes=" + start + "-" + end);
            int code = c.getResponseCode();
            if (code == HttpURLConnection.HTTP_OK) {
                // The server ignored the range and started at zero: segmenting cannot work.
                throw new RangesRefused();
            }
            if (code != HttpURLConnection.HTTP_PARTIAL) throw new IOException("HTTP " + code);
            byte[] buffer = new byte[128 * 1024];
            long at = start;
            try (InputStream in = c.getInputStream();
                 RandomAccessFile out = new RandomAccessFile(part, "rw")) {
                out.seek(start);
                int n;
                while ((n = in.read(buffer)) > 0) {
                    if (cancelled.get()) break;
                    out.write(buffer, 0, n);
                    at += n;
                    long now = held.addAndGet(n);
                    progress.onProgress(now, total);
                }
            }
            return at - start;
        } finally {
            shut(c);
        }
    }

    /** Single stream, appending: for a server that answers a range request with the whole file. */
    private static String stream(String url, File part, long expected, Progress progress,
                                 AtomicBoolean cancelled) {
        String previous = null;
        for (int attempt = 0; attempt < 8; attempt++) {
            if (cancelled.get()) return "已取消";
            try {
                String problem = oneStream(url, part, expected, progress, cancelled);
                if (problem == null) return null;
                if (cancelled.get()) return "已取消";
                if (problem.startsWith("HTTP 4") || problem.contains("Permission denied")) return problem;
                AppLog.i("下载: 第 " + (attempt + 1) + " 次中断: " + problem);
                previous = problem;
            } catch (Throwable t) {
                if (cancelled.get()) return "已取消";
                previous = t.toString();
                AppLog.i("下载: 第 " + (attempt + 1) + " 次失败: " + t);
            }
            try {
                Thread.sleep(Math.min(30_000L, 2_000L << Math.min(attempt, 4)));
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return "已取消";
            }
        }
        return "下载失败: " + previous;
    }

    private static String oneStream(String url, File part, long expected, Progress progress,
                                    AtomicBoolean cancelled) throws IOException {
        long have = part.exists() ? part.length() : 0;
        if (expected > 0 && have > expected) {
            if (!part.delete()) return "无法删除 " + part;
            have = 0;
        }
        if (expected > 0 && have == expected) return null;

        HttpURLConnection c = open(url);
        try {
            Tls.apply(c);
            c.setInstanceFollowRedirects(true);
            c.setConnectTimeout(30_000);
            c.setReadTimeout(120_000);
            c.setRequestProperty("User-Agent", "Henge/Android");
            if (have > 0) c.setRequestProperty("Range", "bytes=" + have + "-");
            int code = c.getResponseCode();
            long total;
            if (code == HttpURLConnection.HTTP_PARTIAL) {
                total = have + c.getContentLengthLong();
            } else if (code == HttpURLConnection.HTTP_OK) {
                total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : expected;
                if (have > 0) {
                    if (!part.delete()) return "无法删除 " + part;
                    have = 0;
                }
            } else {
                return "HTTP " + code;
            }
            if (total <= 0) total = expected;
            progress.onProgress(have, total);

            long received = have;
            byte[] buffer = new byte[256 * 1024];
            try (InputStream in = c.getInputStream();
                 FileOutputStream out = new FileOutputStream(part, true)) {
                int n;
                while ((n = in.read(buffer)) > 0) {
                    if (cancelled.get()) return "已取消";
                    out.write(buffer, 0, n);
                    received += n;
                    progress.onProgress(received, total);
                }
                out.flush();
            }
            if (total > 0 && received < total) return "只收到 " + received + " / " + total + " 字节";
            return null;
        } finally {
            shut(c);
        }
    }

    /**
     * How much of the image is really held, from the chunk marks rather than the file's length.
     *
     * The file is grown to the full size before the first byte is fetched (that is what makes each
     * worker able to write at its own offset), so its length says nothing about progress — with it
     * a download that had just started looked complete.
     */
    static long held(File part, File marks, long total) {
        if (!part.exists()) return 0;
        if (!marks.exists()) return 0;
        try {
            byte[] done = java.nio.file.Files.readAllBytes(marks.toPath());
            long sum = 0;
            int count = (int) ((total + CHUNK - 1) / CHUNK);
            for (int i = 0; i < count && i < done.length; i++)
                if (done[i] == 1) sum += chunkSize(i, total);
            return sum;
        } catch (Throwable t) {
            return 0;
        }
    }

    /** Thrown when a range request came back as the whole file. */
    private static final class RangesRefused extends IOException {
        RangesRefused() {
            super("server ignored Range");
        }
    }

    private static long chunkSize(int index, long total) {
        long start = index * CHUNK;
        return Math.min(total, start + CHUNK) - start;
    }

    private static byte[] readMarks(File marks, int count) throws IOException {
        byte[] done = new byte[count];
        if (!marks.exists() || marks.length() != count) return done;
        try (RandomAccessFile in = new RandomAccessFile(marks, "r")) {
            in.readFully(done);
        }
        return done;
    }

    private static void writeMarks(File marks, byte[] done, int index) {
        try (RandomAccessFile out = new RandomAccessFile(marks, "rw")) {
            if (out.length() != done.length) out.setLength(done.length);
            out.seek(index);
            out.write(done[index]);
        } catch (IOException e) {
            // Losing a mark costs one chunk's worth of re-download, not correctness.
            AppLog.i("下载: 断点记录写入失败（" + e + "）");
        }
    }

    private static void copy(File from, File to) throws IOException {
        try (RandomAccessFile in = new RandomAccessFile(from, "r");
             FileOutputStream out = new FileOutputStream(to)) {
            byte[] buffer = new byte[1 << 20];
            int n;
            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
        }
    }
}
