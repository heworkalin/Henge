package dev.henge.probe;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * The virtual Mac's boot ROM, obtained the way the iOS app obtains it.
 *
 * AVPBooter.vmapple2.bin is not a file inside an IPSW, and that is the whole reason installing used
 * to fail before it started: the installer was looking for an entry called AVPBooter in the archive
 * and there has never been one. The ROM lives inside the system image, as one 1 MiB LZFSE block of
 * a compressed file — hence the offsets below, which are a fixed property of Apple's 14.6.1 restore
 * image and were found by reading its disk layout.
 *
 * So this fetches 860 KB from Apple rather than shipping Apple's binary, and it takes them from the
 * 14.6.1 image specifically: macOS 15 and later are published encrypted, and this ROM boots and
 * installs all of them. The unpacking and the hash check are native ({@link Native}), because they
 * need lzfse and a second LZVN pass Java has no reason to implement.
 *
 * The consequence worth stating plainly: preparing a machine needs the network once, even if the
 * IPSW you are installing from is already on disk. It is under a megabyte.
 */
final class BootRom {

    static final String NAME = "AVPBooter.vmapple2.bin";

    /** The bytes this file must have; a different length means a different file. */
    static final int BYTES = 214_904;

    private static final String SOURCE =
            "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/"
                    + "932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw";
    private static final long BLOCK_OFFSET = 5_349_700_747L;
    private static final int BLOCK_BYTES = 859_848;

    private BootRom() {
    }

    /** True when a plausible ROM is already on disk. The hash is checked when one is unpacked. */
    static boolean present(File dir) {
        File rom = new File(dir, NAME);
        return rom.exists() && rom.length() == BYTES;
    }

    /**
     * Makes sure {@code dir/AVPBooter.vmapple2.bin} exists.
     *
     * Returns null when it does, or a sentence saying why not — this runs on a worker thread and its
     * failures go on screen. Failures are specific on purpose: "the hash did not match" and "the
     * network is down" call for different actions from the user.
     */
    static String prepare(File dir) {
        File rom = new File(dir, NAME);
        if (present(dir)) return null;
        if (!dir.exists() && !dir.mkdirs()) return "无法创建目录 " + dir;

        byte[] block;
        try {
            block = fetchBlock();
        } catch (IOException e) {
            AppLog.w("ROM: 下载失败", e);
            return "取引导 ROM 失败（需要联网，约 860 KB）: " + e;
        }

        String problem;
        try {
            problem = Native.nativeBootRomUnpack(block, rom.getAbsolutePath());
        } catch (Throwable t) {
            AppLog.w("ROM: 解包异常", t);
            return "解包引导 ROM 异常: " + t;
        }
        if (problem != null) {
            AppLog.w("ROM: " + problem, null);
            return "引导 ROM " + problem;
        }
        AppLog.i("ROM: 已解出 " + rom + "（" + rom.length() + " 字节）");
        return null;
    }

    /** One ranged request for the block that holds the ROM. */
    private static byte[] fetchBlock() throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(SOURCE).openConnection();
        try {
            Tls.apply(c);
            c.setInstanceFollowRedirects(true);
            c.setConnectTimeout(30_000);
            c.setReadTimeout(120_000);
            c.setRequestProperty("User-Agent", "Henge/Android");
            c.setRequestProperty("Range",
                    "bytes=" + BLOCK_OFFSET + "-" + (BLOCK_OFFSET + BLOCK_BYTES - 1));
            int code = c.getResponseCode();
            if (code != HttpURLConnection.HTTP_PARTIAL) {
                throw new IOException("HTTP " + code + "（服务器没有按范围返回）");
            }
            byte[] buffer = new byte[BLOCK_BYTES];
            int at = 0;
            try (InputStream in = c.getInputStream()) {
                int n;
                while (at < buffer.length && (n = in.read(buffer, at, buffer.length - at)) > 0) {
                    at += n;
                }
            }
            if (at != buffer.length) throw new IOException("只收到 " + at + " / " + buffer.length);
            AppLog.i("ROM: 已取到压缩块 " + buffer.length + " 字节");
            return buffer;
        } finally {
            c.disconnect();
        }
    }
}
