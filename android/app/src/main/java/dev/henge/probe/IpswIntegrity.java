package dev.henge.probe;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * Checks an IPSW against the checksums it carries for itself.
 *
 * An IPSW is a zip, and every entry has a CRC32 in the central directory. Nothing in the install
 * path checks them: libzip reads entries without verifying, and idevicerestore does not either, so
 * a download that went wrong at one byte would be personalised and sent to the guest as if it were
 * fine — the failure would surface much later, as a firmware that misbehaves for no visible reason.
 *
 * The check is the whole file, not a sample: CRC32 is cheap compared with reading 16 GB, and a
 * sample cannot tell "one sector went bad" from "the file is fine".
 *
 * What it does not do is decide anything. The caller decides whether a bad file is deleted (only
 * one this app downloaded is) or merely reported (one the user brought in never is).
 */
final class IpswIntegrity {

    /** Outcome: `ok` when every entry matched, otherwise the first one that did not. */
    static final class Result {
        final boolean ok;
        final String problem;
        final long bytes;

        Result(boolean ok, String problem, long bytes) {
            this.ok = ok;
            this.problem = problem;
            this.bytes = bytes;
        }
    }

    private IpswIntegrity() {
    }

    /**
     * Streams every entry and compares its CRC32 with the archive's own record.
     *
     * `progress` gets a line to show while this runs — a 16 GB file takes long enough that a frozen
     * screen would look like a hang.
     */
    static Result verify(File ipsw, java.util.function.Consumer<String> progress,
                         AtomicBoolean cancelled) {
        if (!ipsw.exists()) return new Result(false, "文件不存在: " + ipsw, 0);

        // The expected checksums come from the central directory; the streaming reader below cannot
        // report them for entries written with a data descriptor.
        Map<String, Long> expected = new HashMap<>();
        try (ZipFile zip = new ZipFile(ipsw)) {
            java.util.Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry e = entries.nextElement();
                if (e.getCrc() != -1) expected.put(e.getName(), e.getCrc());
            }
        } catch (IOException e) {
            return new Result(false, "压缩包结构损坏: " + e, 0);
        }

        long total = ipsw.length(), read = 0, nextReport = 0;
        byte[] buffer = new byte[1 << 20];
        try (ZipInputStream in = new ZipInputStream(new java.io.BufferedInputStream(
                new FileInputStream(ipsw), 1 << 20))) {
            ZipEntry entry;
            while ((entry = in.getNextEntry()) != null) {
                if (cancelled.get()) return new Result(false, "已取消", read);
                CRC32 crc = new CRC32();
                long size = 0;
                int n;
                while ((n = in.read(buffer)) > 0) {
                    crc.update(buffer, 0, n);
                    size += n;
                    read += n;
                    if (progress != null && read >= nextReport) {
                        nextReport = read + (256L << 20);
                        progress.accept("校验镜像 " + (total > 0 ? read * 100 / total : 0) + "%");
                    }
                    if (cancelled.get()) return new Result(false, "已取消", read);
                }
                Long want = expected.get(entry.getName());
                if (want != null && want != -1 && want != crc.getValue()) {
                    return new Result(false, "条目校验不符: " + entry.getName()
                            + "（应为 " + Long.toHexString(want)
                            + "，实际 " + Long.toHexString(crc.getValue()) + "）", read);
                }
            }
        } catch (IOException e) {
            AppLog.w("校验: 读取失败", e);
            return new Result(false, "读取失败: " + e, read);
        }
        if (progress != null) progress.accept("校验完成");
        AppLog.i("校验: " + ipsw.getName() + " 通过（" + AppLog.bytes(read) + "）");
        return new Result(true, null, read);
    }
}
