package dev.henge.probe;

import android.app.Activity;
import android.content.ContentResolver;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.function.Consumer;

/**
 * Brings an IPSW the user already has into the installer.
 *
 * The picker hands back a content URI, and the installer wants a path — it opens the image with
 * libzip, from native code, to find the restore image and the firmware inside. So the first thing
 * to try is the honest one: a URI backed by a real file has a real path behind its descriptor, and
 * `/proc/self/fd/N` names it. When that path is readable from this process the image is used where
 * it sits, which matters when it is sixteen gigabytes and the internal volume has no room for two
 * copies of it.
 *
 * When it is not — a cloud provider, or scoped storage refusing the path — the file is copied into
 * the app's own directory instead, with progress, because a copy fails slowly and silently
 * otherwise.
 */
final class IpswImport {

    /** What a pick produced: a path the installer can read, or a sentence saying why not. */
    static final class Result {
        final String path;
        final String error;

        Result(String path, String error) {
            this.path = path;
            this.error = error;
        }
    }

    private IpswImport() {
    }

    /**
     * Resolves `uri` to something libzip can open.
     *
     * `progress` is called as bytes move when a copy is needed, and not at all when the file can be
     * read in place.
     */
    static Result take(Activity a, Uri uri, File vmDir, Consumer<String> progress) {
        if (!vmDir.exists() && !vmDir.mkdirs()) return new Result(null, "无法创建 " + vmDir);

        ParcelFileDescriptor pfd = null;
        try {
            ContentResolver resolver = a.getContentResolver();
            pfd = resolver.openFileDescriptor(uri, "r");
            if (pfd == null) return new Result(null, "打不开所选文件");

            // A descriptor backed by a plain file resolves to its path. /proc/self/fd is a symlink
            // to it, so the canonical path is the file's own name — no copy, no second copy's room.
            String inPlace = null;
            try {
                File link = new File("/proc/self/fd/" + pfd.getFd());
                String canonical = link.getCanonicalPath();
                File real = new File(canonical);
                if (real.isFile() && real.canRead() && real.length() > (1L << 30)) {
                    // canRead() is the app's own permission, not the picker's grant: only trust it
                    // when a byte actually comes out.
                    try (FileInputStream probe = new FileInputStream(real)) {
                        if (probe.read() >= 0) inPlace = canonical;
                    }
                }
            } catch (IOException ignored) {
                // Not a plain file, or not readable: fall through to copying.
            }
            if (inPlace != null) {
                AppLog.i("导入: 就地使用 " + inPlace);
                return new Result(inPlace, null);
            }

            String name = displayName(uri);
            File out = new File(vmDir, name != null && name.endsWith(".ipsw") ? name : "imported.ipsw");
            long total = pfd.getStatSize();
            long copied = 0;
            long[] last = {System.currentTimeMillis()};
            try (InputStream in = new FileInputStream(pfd.getFileDescriptor());
                 FileOutputStream fos = new FileOutputStream(out)) {
                byte[] buffer = new byte[1 << 20];
                int n;
                while ((n = in.read(buffer)) > 0) {
                    fos.write(buffer, 0, n);
                    copied += n;
                    long now = System.currentTimeMillis();
                    if (now - last[0] > 500) {
                        last[0] = now;
                        long have = copied;
                        progress.accept("复制中 " + AppLog.bytes(have)
                                + (total > 0 ? " / " + AppLog.bytes(total) : ""));
                    }
                }
            }
            AppLog.i("导入: 已复制到 " + out + "（" + AppLog.bytes(out.length()) + "）");
            return new Result(out.getAbsolutePath(), null);
        } catch (Throwable t) {
            AppLog.w("导入: 失败", t);
            return new Result(null, "导入失败: " + t);
        } finally {
            if (pfd != null) {
                try {
                    pfd.close();
                } catch (IOException ignored) {
                }
            }
        }
    }

    /** The file name the picker knows, if it knows one. */
    private static String displayName(Uri uri) {
        String last = uri.getLastPathSegment();
        if (last == null) return null;
        int slash = last.lastIndexOf('/');
        return slash >= 0 ? last.substring(slash + 1) : last;
    }
}
